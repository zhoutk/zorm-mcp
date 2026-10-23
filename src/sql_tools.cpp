// sql_tools.cpp - the SQL escape hatches:
//   db_query_sql  READ-ONLY parameterized SQL (always registered; statement-
//                 level allowlist keeps it SELECT/PRAGMA/SHOW/EXPLAIN/WITH).
//   db_exec_sql   write/DDL SQL (registered ONLY with --allow-sql; MRTR).
//   db_trans      atomic multi-statement batch via Idb::transGo (MRTR).

#include "app.hpp"
#include "json_util.hpp"
#include "tools.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace zorm_mcp {

using mcp::Json;
using mcp::McpErrorCode;
using mcp::McpResult;
using mcp::member;
using mcp::server::Ctx;

namespace {

// Blanks out everything whose CONTENT is not SQL syntax: string literals,
// quoted identifiers, and comments. Keyword analysis then cannot be fooled by
// "select 'delete from t'" or by a column literally named "update". The mask
// preserves offsets/length; only bytes become spaces (newlines stay, so line
// comments terminate correctly).
std::string maskNonCode(const std::string& sql) {
  std::string out = sql;
  size_t i = 0;
  while (i < out.size()) {
    const char c = out[i];
    if (c == '-' && i + 1 < out.size() && out[i + 1] == '-') {
      while (i < out.size() && out[i] != '\n') out[i++] = ' ';
      continue;
    }
    if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
      out[i++] = ' ';
      out[i++] = ' ';
      while (i < out.size()) {
        if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/') {
          out[i++] = ' ';
          out[i++] = ' ';
          break;
        }
        if (out[i] != '\n') out[i] = ' ';
        ++i;
      }
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      const char quote = c;
      out[i++] = ' ';
      while (i < out.size()) {
        if (out[i] == quote) {
          // Doubled quote is an escaped quote inside the literal.
          if (i + 1 < out.size() && out[i + 1] == quote) {
            out[i++] = ' ';
            out[i++] = ' ';
            continue;
          }
          out[i++] = ' ';
          break;
        }
        if (out[i] != '\n') out[i] = ' ';
        ++i;
      }
      continue;
    }
    if (c == '[') {  // sqlite/tsql bracket identifier
      out[i++] = ' ';
      while (i < out.size()) {
        if (out[i] == ']') {
          out[i++] = ' ';
          break;
        }
        if (out[i] != '\n') out[i] = ' ';
        ++i;
      }
      continue;
    }
    ++i;
  }
  return out;
}

std::vector<std::string> sqlWords(const std::string& masked) {
  std::vector<std::string> words;
  std::string cur;
  for (const char ch : masked) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '_') {
      cur.push_back(static_cast<char>(std::tolower(u)));
    } else if (!cur.empty()) {
      words.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) words.push_back(cur);
  return words;
}

// Tokens with their offsets, for checks that care WHERE a word sits (e.g.
// "the pragma name is the last word before the '(' "). sqlWords() stays the
// cheap word-only form used by the DML scan.
struct SqlToken {
  std::string text;
  size_t pos;
};

std::vector<SqlToken> sqlTokens(const std::string& masked) {
  std::vector<SqlToken> toks;
  size_t i = 0;
  while (i < masked.size()) {
    const unsigned char u = static_cast<unsigned char>(masked[i]);
    if (std::isalnum(u) || masked[i] == '_') {
      const size_t start = i;
      std::string cur;
      while (i < masked.size() &&
             (std::isalnum(static_cast<unsigned char>(masked[i])) || masked[i] == '_')) {
        cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(masked[i]))));
        ++i;
      }
      toks.push_back({cur, start});
      continue;
    }
    ++i;
  }
  return toks;
}

bool isDmlWord(const std::string& w, const std::string& next) {
  // REPLACE is also a scalar function (REPLACE(str, a, b)), so only the
  // statement form "REPLACE INTO" counts as a write.
  if (w == "replace") return next == "into";
  return w == "insert" || w == "update" || w == "delete" || w == "merge";
}

// Returns the first DML keyword in the statement ("" when none). Used for
// WITH and EXPLAIN, whose first keyword alone says nothing about the body.
std::string firstDmlWord(const std::vector<std::string>& words) {
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string next = i + 1 < words.size() ? words[i + 1] : "";
    if (isDmlWord(words[i], next)) return words[i];
  }
  return "";
}

// Words that may appear between EXPLAIN and the statement it wraps. This is a
// skip list only; whatever it returns is then WHITELISTED, so adding a word
// here can never admit a write on its own.
bool isExplainModifier(const std::string& w) {
  static const char* kModifiers[] = {
      "analyze", "analyse", "verbose", "query", "plan", "format", "for",
      "connection", "json", "xml", "yaml", "text", "traditional", "extended",
      "partitions", "costs", "buffers", "timing", "summary", "settings", "wal",
      "on", "off", "true", "false"};
  for (const char* m : kModifiers) {
    if (w == m) return true;
  }
  return false;
}

// The statement keyword a plan wraps. Skips EXPLAIN's modifier clause
// ("ANALYZE", "VERBOSE", "QUERY PLAN", "(ANALYZE, FORMAT JSON)", ...) and
// returns the first token that is not a modifier. Returns "" when EXPLAIN has
// no target statement.
//
// The caller must require the returned kind to be SELECT / WITH / VALUES.
// Scanning for the first SELECT *anywhere* (the previous behaviour) let
// "EXPLAIN ANALYZE CREATE TABLE x AS SELECT ..." through: it matched the
// SELECT inside CREATE TABLE AS and Postgres then created the table. Only the
// token IMMEDIATELY after the modifiers says what the statement is.
std::string explainedStatementKind(const std::string& masked) {
  // MySQL's read-only "EXPLAIN FOR CONNECTION <id>" inspects a running
  // connection instead of wrapping a statement.
  const std::vector<std::string> head = sqlWords(masked);
  if (head.size() >= 3 && head[0] == "explain" && head[1] == "for" &&
      head[2] == "connection") {
    return "connection";
  }
  size_t i = 0;
  // Skip EXPLAIN itself.
  while (i < masked.size() && !std::isalnum(static_cast<unsigned char>(masked[i])) &&
         masked[i] != '_') {
    ++i;
  }
  while (i < masked.size() &&
         (std::isalnum(static_cast<unsigned char>(masked[i])) || masked[i] == '_')) {
    ++i;
  }
  while (i < masked.size()) {
    const unsigned char u = static_cast<unsigned char>(masked[i]);
    if (std::isspace(u) || masked[i] == ',' || masked[i] == '=') {
      ++i;
      continue;
    }
    if (masked[i] == '(') {  // "(ANALYZE, FORMAT JSON)" - skip the whole group
      int depth = 1;
      ++i;
      while (i < masked.size() && depth > 0) {
        if (masked[i] == '(') ++depth;
        else if (masked[i] == ')') --depth;
        ++i;
      }
      continue;
    }
    if (std::isalnum(u) || masked[i] == '_') {
      std::string word;
      while (i < masked.size() &&
             (std::isalnum(static_cast<unsigned char>(masked[i])) || masked[i] == '_')) {
        word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(masked[i]))));
        ++i;
      }
      if (isExplainModifier(word)) continue;
      // Numeric arguments such as EXPLAIN FOR CONNECTION 42.
      if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0]))) continue;
      return word;
    }
    ++i;
  }
  return "";
}

// PRAGMA is only read-only for a known set of names. `PRAGMA user_version = 42`,
// `PRAGMA journal_mode = wal` and `PRAGMA writable_schema = on` all persist file-
// level changes, and sqlite accepts the same value via `PRAGMA name(value)`, so
// assignment is detected by '=' AND by parentheses on names that do not take an
// argument. Unknown names are refused (fail closed) with a pointer to
// db_exec_sql.
bool pragmaIsReadOnly(const std::string& masked) {
  if (masked.find('=') != std::string::npos) return false;
  const std::vector<SqlToken> toks = sqlTokens(masked);  // toks[0] == "pragma"
  const size_t paren = masked.find('(');
  std::string name;
  if (paren != std::string::npos) {
    for (const SqlToken& t : toks) {
      if (t.pos >= paren) break;
      if (t.text != "pragma") name = t.text;  // schema-qualified: last word wins
    }
    static const char* kArgRead[] = {
        "table_info",    "table_xinfo",     "table_list",      "index_list",
        "index_info",    "index_xinfo",     "foreign_key_list", "foreign_key_check",
        "integrity_check", "quick_check"};
    for (const char* n : kArgRead) {
      if (name == n) return true;
    }
    return false;  // PRAGMA name(value) is an assignment for everything else
  }
  for (const SqlToken& t : toks) {
    if (t.text != "pragma") name = t.text;
  }
  static const char* kBareRead[] = {
      "user_version",   "schema_version",  "data_version",    "journal_mode",
      "page_count",     "page_size",       "freelist_count",  "encoding",
      "database_list",  "collation_list",  "function_list",   "module_list",
      "pragma_list",    "compile_options", "application_id",  "auto_vacuum",
      "cache_size",     "cache_spill",     "checkpoint_fullfsync", "defer_foreign_keys",
      "fullfsync",      "max_page_count",  "mmap_size",       "read_uncommitted",
      "recursive_triggers", "reverse_unordered_selects", "secure_delete",
      "short_column_names", "full_column_names", "count_changes", "temp_store",
      "locking_mode",   "synchronous",     "wal_autocheckpoint", "query_only",
      "trusted_schema", "foreign_keys",    "ignore_check_constraints",
      "legacy_alter_table", "table_info",  "table_xinfo",     "table_list",
      "index_list",     "index_info",      "index_xinfo",     "foreign_key_list",
      "foreign_key_check", "integrity_check", "quick_check"};
  for (const char* n : kBareRead) {
    if (name == n) return true;
  }
  return false;
}

McpResult<Json> unknownDb(const std::string& dbName) {
  return mcp::Err(McpErrorCode::InvalidParams,
                  "unknown db: " + dbName + " (available: " + app().dbListText() +
                      ") - `db` is the backend NAME, see the server instructions");
}

// Read-only statement gate. Allowed first keywords: SELECT / WITH (read
// queries and CTEs), PRAGMA / SHOW / EXPLAIN / DESCRIBE / DESC (introspection
// and plans). Semicolons are rejected outright - one statement per call, no
// statement smuggling.
//
// WITH and EXPLAIN get a SECOND check because their first keyword proves
// nothing: "WITH c AS (SELECT 1) DELETE FROM t" and "EXPLAIN ANALYZE UPDATE"
// both write data (the latter on PostgreSQL) while looking like introspection.
// The body is scanned for a DML keyword after masking comments and quoted
// text. Anything that fails belongs in db_exec_sql (if enabled).
McpResult<Json> checkReadOnlySql(const std::string& sql) {
  size_t i = 0;
  // Skip leading whitespace AND comments: "/* c */ select 1" is a read
  // statement, not a write (D-11). Only the PREFIX is stripped; comments in
  // the middle do not change the first keyword.
  while (true) {
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
      const size_t nl = sql.find('\n', i + 2);
      i = nl == std::string::npos ? sql.size() : nl + 1;
      continue;
    }
    if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
      const size_t end = sql.find("*/", i + 2);
      if (end == std::string::npos) {
        return mcp::Err(McpErrorCode::InvalidParams,
                        "db_query_sql: unterminated block comment");
      }
      i = end + 2;
      continue;
    }
    break;
  }
  static const char* kAllowed[] = {"select", "with", "pragma", "show",
                                   "explain", "describe", "desc"};
  for (const char* kw : kAllowed) {
    size_t j = 0;
    while (kw[j] != '\0') {
      if (i + j >= sql.size() ||
          std::tolower(static_cast<unsigned char>(sql[i + j])) != kw[j]) break;
      ++j;
    }
    if (kw[j] == '\0' &&
        (i + j >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[i + j])))) {
      // keyword matched and is not a prefix of a longer identifier
      if (sql.find(';') != std::string::npos) {
        return mcp::Err(McpErrorCode::InvalidParams,
                        "db_query_sql is READ-ONLY and accepts a single statement "
                        "(no ';'). Multi-statement work belongs in db_trans, write "
                        "SQL in db_exec_sql (--allow-sql)");
      }
      // The first keyword alone is NOT enough: "WITH c AS (...) DELETE" and
      // "EXPLAIN ANALYZE UPDATE" both hit the database for real. Analyze the
      // statement body (comments/literals masked out so they cannot hide it).
      const std::string masked = maskNonCode(sql);
      const std::vector<std::string> words = sqlWords(masked);
      const std::string first = std::string(kw);
      if (first == "with" || first == "explain") {
        std::string dml;
        if (first == "with") {
          dml = firstDmlWord(words);
        } else {
          // EXPLAIN wraps exactly one statement: only the token right after
          // its modifiers decides. Anything but SELECT / WITH / VALUES is
          // refused - CREATE TABLE AS, EXECUTE, CALL, DECLARE, DML, ...
          // (the old scan accepted them by finding a later "select").
          const std::string kind = explainedStatementKind(masked);
          if (kind == "with") {
            dml = firstDmlWord(words);
          } else if (kind == "select" || kind == "values" || kind == "connection") {
            dml.clear();
          } else {
            return mcp::Err(
                McpErrorCode::InvalidParams,
                "db_query_sql is READ-ONLY: EXPLAIN must wrap a SELECT / WITH / "
                "VALUES query, not '" +
                    (kind.empty() ? std::string("<empty>") : kind) +
                    "'. EXPLAIN ANALYZE runs the statement, so a CREATE TABLE AS, "
                    "EXECUTE, CALL or DML there would really execute. Use "
                    "db_exec_sql when the server runs with --allow-sql");
          }
        }
        if (!dml.empty()) {
          return mcp::Err(
              McpErrorCode::InvalidParams,
              "db_query_sql is READ-ONLY: '" + first + " ... " + dml +
                  "' executes a write" +
                  (first == "explain"
                       ? " (EXPLAIN ANALYZE actually runs the statement)"
                       : " (a data-modifying CTE)") +
                  ". Use db_insert / db_update / db_delete / db_trans, or "
                  "db_exec_sql when the server runs with --allow-sql");
        }
      }
      // PRAGMA can write: `PRAGMA user_version = 42` and `PRAGMA journal_mode =
      // wal` persist to the database file without going through any write tool.
      if (first == "pragma" && !pragmaIsReadOnly(masked)) {
        return mcp::Err(
            McpErrorCode::InvalidParams,
            "db_query_sql is READ-ONLY: this PRAGMA assigns a value or is not a "
            "known read-only pragma (allowed: table_info/table_xinfo/table_list/"
            "index_list/foreign_key_list/integrity_check and state readers such "
            "as user_version, journal_mode, page_count). Use db_exec_sql when "
            "the server runs with --allow-sql");
      }
      // SELECT ... INTO OUTFILE/DUMPFILE writes files on MySQL.
      if (first == "select") {
        for (size_t w = 0; w + 1 < words.size(); ++w) {
          if (words[w] == "into" &&
              (words[w + 1] == "outfile" || words[w + 1] == "dumpfile")) {
            return mcp::Err(McpErrorCode::InvalidParams,
                            "db_query_sql is READ-ONLY: SELECT ... INTO " +
                                words[w + 1] +
                                " writes a file. Use a structured write tool or "
                                "db_exec_sql (--allow-sql)");
          }
        }
      }
      return mcp::Ok(Json());
    }
  }
  return mcp::Err(McpErrorCode::InvalidParams,
                  "db_query_sql is READ-ONLY: only SELECT / WITH / PRAGMA / SHOW / "
                  "EXPLAIN / DESCRIBE statements are allowed. To write data use "
                  "db_insert / db_update / db_delete / db_trans, or db_exec_sql "
                  "when the server runs with --allow-sql");
}

}  // namespace

McpResult<Json> toolQuerySql(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  const std::string sql = member(args, "sql").toString();
  if (sql.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "sql is required");
  }
  if (const McpResult<Json> gate = checkReadOnlySql(sql); gate.has_value() == false) {
    return gate;
  }

  // values: positional array bound to the placeholders in order (zorm
  // querySql(sql, params, values) - named params stay available for power
  // users via `params`).
  Json values = member(args, "values");
  if (!values.isArray()) values = Json("[]");
  // DbBase's facade requires params to be an OBJECT - a missing `params`
  // argument must become an empty object, never a null Json.
  ZORM::Json zparams;
  const Json namedParams = member(args, "params");
  if (namedParams.isObject()) zparams = toZormJson(namedParams);

  // PostgreSQL only understands $n placeholders. Normalize the documented `?`
  // form so one SQL shape works on every backend (D-07). The rewrite skips
  // literals/comments/dollar-quotes; a count mismatch is a clear user error.
  std::string sqlText = sql;
  if (app().specs[dbName].type == "postgres" && values.size() > 0) {
    const size_t n = rewritePgPlaceholders(sqlText);
    if (n > 0 && n != static_cast<size_t>(values.size())) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "SQL has " + std::to_string(n) +
                          " '?' placeholders but `values` has " +
                          std::to_string(values.size()) +
                          " entries (values must match placeholders 1:1)");
    }
  }

  const ZORM::Json rs = db->querySql(sqlText, zparams, toZormJson(values));
  if (!okStatus(rs)) {
    std::string msg = rs["message"].toString();
    if (app().specs[dbName].type == "dm8") msg = toUtf8FromGbk(msg);
    return mcp::Err(McpErrorCode::InvalidParams, "query failed: " + msg);
  }
  return mcp::Ok(toolResult(toMcpJson(rs)));
}

McpResult<Json> toolExecSql(Ctx& c) {
  if (!app().allowSql) {
    return mcp::Err(McpErrorCode::MethodNotFound,
                    "db_exec_sql is not enabled - restart the server with "
                    "--allow-sql. Structured writes (db_insert/db_update/db_delete/"
                    "db_trans) work without it");
  }
  return stageWrite(c, "exec_sql");
}

McpResult<Json> toolTrans(Ctx& c) { return stageWrite(c, "trans"); }

void registerSqlTools(mcp::server::Builder& b) {
  b.tool(Json(R"json({"name":"db_query_sql","description":"Ad-hoc READ-ONLY SQL for what db_query cannot express: JOINs, dialect functions, EXPLAIN plans. Only SELECT / WITH / PRAGMA / SHOW / EXPLAIN / DESCRIBE, one statement, no ';'. WITH and EXPLAIN bodies are checked: 'WITH ... DELETE/UPDATE/INSERT' is rejected, and EXPLAIN must wrap SELECT/WITH/VALUES ('EXPLAIN ANALYZE CREATE TABLE AS', EXECUTE, CALL and DML really execute and are rejected). PRAGMA accepts read-only forms only (table_info, index_list, user_version, journal_mode, ...); assignments such as 'PRAGMA user_version = 42' need db_exec_sql. Bind values via the values array (? placeholders, in order) instead of string concatenation. Writes belong in db_insert / db_update / db_delete / db_trans.","inputSchema":{"type":"object","required":["db","sql"],"properties":{"db":{"type":"string","description":"backend name"},"sql":{"type":"string","description":"single read-only statement"},"values":{"type":"array","items":{},"description":"positional bindings for ? placeholders"},"params":{"type":"object","description":"named parameters (zorm querySql semantics)"}}}})json"),
         toolQuerySql);
  b.tool(Json(R"({"name":"db_trans","description":"ATOMIC multi-statement transaction (MRTR-confirmed). sqls is a non-empty array; each element is either {\"text\": \"sql\", \"values\": [...]} (or \"sql\" instead of \"text\") or structured {\"table\", \"method\": \"insert\"|\"update\"|\"delete\"|\"batch\", \"params\": {...}, \"id\": v}. All statements commit together or all roll back. This is also the escape hatch for condition-based updates/deletes that the single-row write tools cannot express.","inputSchema":{"type":"object","required":["db","sqls"],"properties":{"db":{"type":"string"},"sqls":{"type":"array","items":{"type":"object"},"minItems":1}}}})"),
         toolTrans);
  // db_exec_sql is registered separately by buildServer() when --allow-sql is
  // on - hidden by default so AI clients never see a raw-write tool unless the
  // operator opted in.
}

}  // namespace zorm_mcp
