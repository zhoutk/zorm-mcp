// guide.cpp - see guide.hpp. This file IS the user manual; keep every claim
// in sync with the handlers in meta_tools.cpp / data_tools.cpp / sql_tools.cpp.

#include "guide.hpp"
#include "app.hpp"

namespace zorm_mcp {

using mcp::Json;
using mcp::server::Builder;
using mcp::server::Ctx;

std::string instructionsText() {
  return
      "Multi-backend database assistant over zorm - one tool semantics for every "
      "backend, no SQL dialect knowledge required.\n"
      "Backends: {" + app().dbListText() + "}. Every tool takes a `db` routing "
      "parameter = the backend NAME from that list (not a file name).\n"
      "\n"
      "Tool selection:\n"
      "- First contact with a database: db_list_tables {db}, then "
      "db_describe_table {db, table} for columns/types/primary keys (works on "
      "empty tables).\n"
      "- Query data WITHOUT SQL: db_query (preferred). Structured params: fields, "
      "lks (fuzzy, % wildcards), ins (IN; an array value means multiple values "
      "for that column, e.g. {\"age\":[18,20]}), ors (OR), group, sum, count, "
      "sort, page/size, plus plain column conditions - values compare equal, or "
      "use a prefix string like \">,100\" (>, >=, <, <=, <>, =). count/sum take "
      "{\"col\":\"alias\"}, [\"col\"] or \"col,alias\". size may be used alone "
      "(page 1); page requires size.\n"
      "- Row count only: db_count {db, table, where?}.\n"
      "- Ad-hoc READ-ONLY SQL (JOINs, dialect functions, EXPLAIN): db_query_sql. "
      "SELECT/PRAGMA/SHOW/EXPLAIN/WITH only; pass values as a `values` array "
      "bound to ? placeholders.\n"
      "- Writes: db_insert (one row), db_insert_batch (multi-row upsert on the "
      "constraint column), db_update (single row; where must carry id), db_delete "
      "(where {\"id\": v} = one row, where {} = ALL rows), db_trans (atomic "
      "multi-statement; elements {text|sql, values} or {table, method, params, id}).\n"
      "\n"
      "MRTR write confirmation: the FIRST call of every write tool does NOT "
      "execute. It returns resultType=input_required with a requestState. To "
      "execute, re-call the same tool with the original arguments PLUS the "
      "requestState and inputResponses {\"confirm\": {\"approved\": true}} at "
      "the CALL-PARAMS level (siblings of `arguments`, as the MCP client library "
      "does automatically) - or approved:false to cancel. requestState is an "
      "opaque, integrity-protected token: echo it verbatim, never edit or reuse "
      "it for another tool. If the client cannot send params-level fields, the "
      "degraded path is the original arguments + "
      "arguments.inputResponses {\"confirm\":{\"approved\":true}} WITHOUT a "
      "requestState (the server uses the latest matching pending op; trusted "
      "clients only). Tell the user what will be written before asking.\n"
      "\n"
      "Identifiers in structured tools (table, columns, sort, aliases, data "
      "keys) must be simple names: letters/digits/underscore, not starting with "
      "a digit. Complex names need db_query_sql/db_exec_sql.\n"
      "\n"
      "Result shape: {status, data, ...} from zorm. status 200 = ok, 202 = empty "
      "result (NOT an error). data is an array of row objects.\n"
      "When a call fails, the error text tells you the next move - follow it "
      "(e.g. an unknown db lists the available backends).";
}

std::string playbookText() {
  return
      "zorm-mcp playbook - full usage manual\n"
      "\n"
      "1. Backends\n"
      "   Every tool takes `db` = a backend name. Discover them from the server "
      "instructions or by calling db_info on a known name; a wrong name returns "
      "the list of valid ones.\n"
      "\n"
      "2. Schema discovery (no data needed)\n"
      "   db_list_tables {db} -> [tables]\n"
      "   db_describe_table {db, table} -> columns: [{name, type, nullable, "
      "default, primary_key}] - served from catalog metadata (pragma "
      "table_info / information_schema.columns / user_tab_columns / the jsonfile "
      "columns array), so EMPTY tables report full structure.\n"
      "   db_indexes {db, table} -> index list (best-effort per dialect).\n"
      "   db_info {db} -> backend type, server version, table count.\n"
      "\n"
      "3. Structured query (preferred over SQL)\n"
      "   db_query {db, table, fields?, lks?, ins?, ors?, group?, sum?, count?, "
      "sort?, page?, size?, ...column conditions}\n"
      "   - lks: {col:\"%abc%\"} fuzzy match; ins: {col:[v1,v2]} means ONE "
      "column with multiple values (IN-list); ors: OR of conditions.\n"
      "   - group + sum/count: {group:[\"region\"], sum:{amount:\"total\"}} or "
      "sum:\"amount,total\"; count:\"*,total\"; alias = result key.\n"
      "   - column conditions: {price:100} equal; {price:\">,100\"} compares "
      "(prefixes >, >=, <, <=, <>, =).\n"
      "   - page/size: pagination (size 1..500). size alone means page 1; page "
      "requires size (otherwise rejected). The result carries records/pages.\n"
      "   - identifiers: table/columns/sort/aliases are simple names "
      "(letters/digits/underscore). Complex names need db_query_sql.\n"
      "\n"
      "4. SQL escape hatch\n"
      "   db_query_sql {db, sql, values?} - READ-ONLY statements only "
      "(SELECT/PRAGMA/SHOW/EXPLAIN/WITH). values: array bound to ? placeholders "
      "in order (the server rewrites ? to $1..$n for postgres). Leading "
      "comments are allowed; multiple statements are not. WITH and EXPLAIN are "
      "body-checked: data-modifying CTEs (WITH ... DELETE) and EXPLAIN ANALYZE "
      "of a write are rejected - they do write data. JOINs and dialect "
      "functions live here; everything else should prefer db_query.\n"
      "   db_exec_sql {db, sql} - write/DDL SQL, only when the server runs with "
      "--allow-sql, and always behind MRTR confirmation.\n"
      "\n"
      "5. Writes (all MRTR-confirmed)\n"
      "   db_insert {db, table, data} - one row; missing id is auto-generated.\n"
      "   db_insert_batch {db, table, rows, constraint?} - multi-row upsert on "
      "the constraint column (default id).\n"
      "   db_update {db, table, data, where:{id}} - single-row update.\n"
      "   db_delete {db, table, where} - {\"id\": v} one row; {} ALL rows "
      "(destructive; the confirmation states the affected count).\n"
      "   db_trans {db, sqls} - atomic batch; element shapes: {\"text\": sql, "
      "\"values\": [...]} (or \"sql\"), or structured {\"table\", \"method\": "
      "insert|update|delete|batch, \"params\": {...}, \"id\": v}.\n"
      "\n"
      "6. MRTR confirmation flow\n"
      "   Call a write tool -> resultType=input_required, an opaque "
      "requestState, and a message naming db/table/impact. Show it to the user. "
      "Re-call with the original arguments + requestState + inputResponses "
      "{\"confirm\": {\"approved\": true|false}} at the CALL-PARAMS level (not "
      "inside arguments). The state is HMAC-sealed and bound to the tool (and, "
      "over HTTP, to the authenticated bearer), so a forged, edited, expired, "
      "or cross-tool state is rejected -> call again without them to stage a "
      "fresh one.\n"
      "\n"
      "7. Result envelope\n"
      "   zorm status codes: 200 ok, 202 empty result (not an error), 3xx/7xx "
      "errors (message explains). SQLite driver errors are annotated "
      "(READONLY/BUSY/CORRUPT/FULL/CANTOPEN) and name the exact file being "
      "written.";
}

void registerPlaybookPrompt(Builder& b) {
  Json def = Json(R"({"name":"db-playbook","description":"Full usage manual for the zorm-mcp tools: tool selection, smart-query parameters, MRTR confirmation flow, transaction shapes, result envelope.","arguments":[]})");
  b.prompt(def, [](Ctx&) {
    Json msg;
    msg.add("role", Json::str("user"));
    Json content;
    content.add("type", Json::str("text"));
    content.add("text", Json::str(playbookText()));
    msg.add("content", std::move(content));
    Json messages = Json("[]");
    messages.push_back(std::move(msg));
    Json result;
    result.add("description",
               Json::str("zorm-mcp full usage manual (playbook)"));
    result.add("messages", std::move(messages));
    return mcp::Ok(std::move(result));
  });
}

mcp::McpResult<Json> resourceGuide(Ctx&) {
  Json result;
  Json contents = Json("[]");
  Json item;
  item.add("uri", Json::str("db://guide"));
  item.add("text", Json::str(playbookText()));
  contents.push_back(std::move(item));
  result.add("contents", std::move(contents));
  return mcp::Ok(std::move(result));
}

}  // namespace zorm_mcp
