// app.cpp - zorm-mcp application state: config, backend registry, startup
// schema discovery, and the per-dialect catalog routing behind listTables /
// describeTable.
//
// Metadata strategy (why this works on EMPTY tables): SQL backends are
// introspected through their catalogs via querySql - sqlite pragma
// table_info, information_schema.columns on mysql/postgres, user_tab_columns
// on dm8 - never by "select a row and guess columns". jsonfile has no SQL;
// its storage format keeps a per-table "columns" array which we read
// directly (docs/jsonfile-design.md §1).
//
// Identifier safety: catalog predicates inline the table name ONLY after
// validIdentifier() (see json_util.hpp). Everything user-supplied elsewhere
// goes through zorm's parameterized paths.

#include "app.hpp"
#include "DbBase.h"
#include "json_util.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>

namespace zorm_mcp {

App& app() {
  static App g;
  return g;
}

// ---- config ---------------------------------------------------------------------

namespace {

// Built-in open-box defaults: one sqlite file + one json file in the working
// directory, zero external servers. DbLogClose:true keeps zorm's SQL logs off
// stdout - MCP stdio requires stdout to carry ONLY JSON-RPC messages.
std::map<std::string, DbSpec> defaultSpecs() {
  std::map<std::string, DbSpec> dbs;
  dbs["sqlite3"] = {"sqlite3", ZORM::Json(
      R"({"connString":"zorm-mcp.db","DbLogClose":true,"parameterized":true})")};
  // NOT "zorm-mcp.json": that name is the five-backend CONFIG template in the
  // repo root. Using it as the default data file made the open-box server read
  // its own config as an empty database and corrupt the template on the first
  // write (D-01).
  dbs["jsonfile"] = {"jsonfile", ZORM::Json(
      R"({"connString":"zorm-mcp-data.json","DbLogClose":true})")};
  return dbs;
}

// Ensures stdout purity and parameterized behavior regardless of what a user
// config file says (or omits). Users can override `parameterized` explicitly.
// `DbLogClose` is forced in stdio mode - a stray log line corrupts the stream;
// HTTP mode keeps user-controlled logging so generated SQL can be observed.
ZORM::Json sanitizeOptions(ZORM::Json options, bool allowLogs) {
  if (!allowLogs || !ZJSON::hasChild(options, "DbLogClose"))
    options.add("DbLogClose", true);
  if (!ZJSON::hasChild(options, "parameterized")) options.add("parameterized", true);
  return options;
}

}  // namespace

bool loadSpecs(const std::string& configPath, bool allowLogs) {
  App& g = app();
  g.specs = defaultSpecs();
  if (!allowLogs) {
    for (auto& [name, spec] : g.specs) spec.options = sanitizeOptions(spec.options, false);
  }
  if (configPath.empty()) return true;

  std::ifstream in(configPath, std::ios::binary);
  if (!in) {
    std::cerr << "[zorm-mcp] config file not readable: " << configPath << std::endl;
    return false;
  }
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string err;
  mcp::Json cfg = mcp::Json::ParseJson(raw, err);
  const mcp::Json dbs = mcp::member(cfg, "databases");
  if (cfg.isError() || !dbs.isObject()) {
    std::cerr << "[zorm-mcp] bad config (expected a \"databases\" object): "
              << configPath << std::endl;
    return false;
  }
  g.specs.clear();
  for (const auto& k : dbs.getAllKeys().toVector()) {
    const std::string name = k.toString();
    const mcp::Json v = dbs[name];
    DbSpec spec;
    spec.type = mcp::member(v, "type").toString();
    if (spec.type.empty()) {
      std::cerr << "[zorm-mcp] config: database '" << name << "' has no type" << std::endl;
      return false;
    }
    const mcp::Json opts = mcp::member(v, "options");
    spec.options = sanitizeOptions(opts.isObject()
                                       ? toZormJson(opts)
                                       : ZORM::Json(R"({})"), allowLogs);
    g.specs[name] = std::move(spec);
  }
  return true;
}

namespace {

// Refuses to use a zorm-mcp CONFIG file as the jsonfile data store. The
// template (`{"databases":{...}}`) parses as a JSON object, while the jsonfile
// backend expects an array of table objects; opening it silently produced an
// empty database and zorm then backed up the config as `.corrupt.*` on the
// first write (D-01).
bool jsonfilePathLooksLikeConfig(const ZORM::Json& options) {
  if (!ZJSON::hasChild(options, "connString")) return false;
  const std::string file = options["connString"].toString();
  if (file.empty() || file == "null") return false;
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::string raw((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  std::string err;
  const mcp::Json doc = mcp::Json::ParseJson(raw, err);
  return !doc.isError() && doc.isObject() && ZJSON::hasChild(doc, "databases");
}

}  // namespace

bool openBackends() {
  App& g = app();
  for (const auto& [dbName, spec] : g.specs) {
    if (spec.type == "jsonfile" && jsonfilePathLooksLikeConfig(spec.options)) {
      std::cerr << "[zorm-mcp] backend '" << dbName
                << "': connString points at a zorm-mcp CONFIG file ("
                << spec.options["connString"].toString()
                << ") - refusing to use it as a database. Point it at a "
                   "separate data file (e.g. zorm-mcp-data.json)."
                << std::endl;
      return false;
    }
    auto db = std::unique_ptr<ZORM::Idb>(new ZORM::DbBase(spec.type, spec.options));
    if (!db) {
      std::cerr << "[zorm-mcp] failed to open backend '" << dbName << "' ("
                << spec.type << ")" << std::endl;
      return false;
    }
    std::cerr << "[zorm-mcp] backend '" << dbName << "' (" << spec.type
              << ") opened" << std::endl;
    g.backends[dbName] = std::move(db);
  }
  return true;
}

// ---- jsonfile storage access -------------------------------------------------------

namespace {

// jsonfile layout: [{"table": "...", "columns": [...], "rows": [...]}, ...]
// (docs/jsonfile-design.md §1). Read straight from the file - the backend has
// no "list tables" API and tables are created dynamically on first write.
// Rows cross the scope boundary as SERIALIZED TEXT: zjson nodes alias their
// parse arena, so keeping a sub-Json alive past `doc`'s scope is dangling.
struct JsonTableEntry {
  std::string table;
  std::vector<std::string> columns;
  std::string rowsText;  // serialized rows array ("" when absent)
};

std::vector<JsonTableEntry> jsonfileTables(const ZORM::Json& options) {
  std::vector<JsonTableEntry> out;
  const std::string file = options["connString"].toString();
  std::ifstream in(file, std::ios::binary);
  if (!in) return out;  // missing/unreadable file = empty database
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string err;
  mcp::Json doc = mcp::Json::ParseJson(raw, err);
  if (doc.isError() || !doc.isArray()) return out;
  for (const auto& item : doc.toVector()) {
    if (!item.isObject() || !ZJSON::hasChild(item, "table")) continue;
    JsonTableEntry e;
    e.table = item["table"].toString();
    const mcp::Json cols = mcp::member(item, "columns");
    if (cols.isArray()) {
      for (const auto& c : cols.toVector()) e.columns.push_back(c.toString());
    }
    const mcp::Json rows = mcp::member(item, "rows");
    if (rows.isArray()) e.rowsText = rows.toString();
    out.push_back(std::move(e));
  }
  return out;
}

// Infers a loose type name for a jsonfile column by scanning a few rows.
// jsonfile is weakly typed ("20" == 20), so this is a hint, not a contract.
std::string jsonfileInferType(const mcp::Json& rows, const std::string& col) {
  if (!rows.isArray()) return "";
  size_t scanned = 0;
  for (const auto& row : rows.toVector()) {
    if (scanned++ >= 20) break;
    if (!row.isObject() || !ZJSON::hasChild(row, col.c_str())) continue;
    const mcp::Json v = row[col.c_str()];
    if (v.isNull()) continue;
    if (v.isNumber()) return "number";
    if (v.isTrue() || v.isFalse()) return "boolean";
    if (v.isArray()) return "array";
    if (v.isObject()) return "object";
    return "string";
  }
  return "";
}

// ---- SQL catalog helpers -----------------------------------------------------------

// Runs a metadata query and returns its rows (empty on any failure - metadata
// is best-effort; a catalog query must never break a tool call).
std::vector<mcp::Json> catalogRows(ZORM::Idb* db, const std::string& sql) {
  std::vector<mcp::Json> rows;
  const ZORM::Json rs = db->querySql(sql);
  if (okStatus(rs)) {
    const mcp::Json data = toMcpJson(rs["data"]);
    if (data.isArray()) rows = data.toVector();
  }
  return rows;
}

// dm8: zorm qualifies every generated statement with db_name as the schema
// ("dbtest"."table"). Catalogs must query that same owner; the login user's
// default schema (e.g. SYSDBA) may be different. Empty for other backends.
std::string dm8Schema(const std::string& dbName) {
  App& g = app();
  const auto it = g.specs.find(dbName);
  if (it == g.specs.end() || it->second.type != "dm8") return "";
  if (!ZJSON::hasChild(it->second.options, "db_name")) return "";
  const std::string s = it->second.options["db_name"].toString();
  return s == "null" ? "" : s;
}

std::vector<std::string> sqlListTables(ZORM::Idb* db, const std::string& type,
                                       const std::string& schema) {
  std::vector<std::string> tables;
  std::string sql;
  if (type == "sqlite3") {
    sql = "select name from sqlite_master where type='table' and name not like 'sqlite_%'";
  } else if (type == "mysql") {
    sql = "select table_name from information_schema.tables "
          "where table_schema = database() and table_type = 'BASE TABLE'";
  } else if (type == "postgres") {
    sql = "select table_name from information_schema.tables "
          "where table_schema = current_schema() and table_type = 'BASE TABLE'";
  } else {  // dm8
    // zorm qualifies every generated statement with db_name as the SCHEMA
    // ("dbtest"."table"). The login user's current schema can differ
    // (SYSDBA), so user_tables would list a different set than the tools can
    // address. Query the same owner zorm targets.
    if (!schema.empty() && validIdentifier(schema)) {
      sql = "select table_name from all_tables where upper(owner) = upper('" +
            schema + "')";
    } else {
      sql = "select table_name from user_tables";
    }
  }
  for (const auto& row : catalogRows(db, sql)) {
    tables.push_back(rowStr(row, {"table_name", "TABLE_NAME", "name", "NAME"}));
  }
  return tables;
}

// pk columns of one table (best-effort per dialect; sqlite gets them from
// pragma table_info itself, so it is not routed here).
std::vector<std::string> sqlPrimaryKeys(ZORM::Idb* db, const std::string& type,
                                        const std::string& table,
                                        const std::string& schema) {
  std::vector<std::string> pks;
  if (type == "mysql") {
    for (const auto& row : catalogRows(
             db, "select column_name from information_schema.columns "
                 "where table_schema = database() and table_name = '" + table +
                     "' and column_key = 'PRI'")) {
      pks.push_back(rowStr(row, {"column_name", "COLUMN_NAME"}));
    }
  } else if (type == "postgres") {
    for (const auto& row : catalogRows(
             db, "select kcu.column_name from information_schema.table_constraints tc "
                 "join information_schema.key_column_usage kcu "
                 "on tc.constraint_name = kcu.constraint_name "
                 "and tc.table_schema = kcu.table_schema "
                 "where tc.table_schema = current_schema() and tc.table_name = '" +
                     table + "' and tc.constraint_type = 'PRIMARY KEY'")) {
      pks.push_back(rowStr(row, {"column_name", "COLUMN_NAME"}));
    }
  } else if (type == "dm8") {
    // CASE_SENSITIVE=Y folds unquoted identifiers to upper case; a table
    // created through zorm (quoted-lowercase DDL) is stored lowercase.
    // upper() = upper() matches either form. When db_name names a schema,
    // query all_* so the login user's own schema does not leak in.
    std::string sql;
    if (!schema.empty() && validIdentifier(schema)) {
      sql = "select cc.column_name from all_constraints c "
            "join all_cons_columns cc on c.owner = cc.owner "
            "and c.constraint_name = cc.constraint_name "
            "where c.constraint_type = 'P' and upper(c.owner) = upper('" +
            schema + "') and upper(c.table_name) = upper('" + table + "')";
    } else {
      sql = "select cc.column_name from user_constraints c "
            "join user_cons_columns cc on c.constraint_name = cc.constraint_name "
            "where c.constraint_type = 'P' and upper(c.table_name) = upper('" +
            table + "')";
    }
    for (const auto& row : catalogRows(db, sql)) {
      pks.push_back(rowStr(row, {"column_name", "COLUMN_NAME"}));
    }
  }
  return pks;
}

std::vector<ColumnMeta> sqlDescribe(ZORM::Idb* db, const std::string& type,
                                    const std::string& table,
                                    const std::string& schema) {
  std::vector<ColumnMeta> cols;
  if (type == "sqlite3") {
    // Table-valued function form, NOT "pragma table_info(...)": the sqlite
    // backend's column-discovery probe appends " limit 1" to metadata
    // queries, which bare pragma statements reject (probe fails -> no
    // columns decoded). The TVF is a plain select, so it survives the probe.
    for (const auto& row : catalogRows(
             db, "select name, type, \"not null\" as \"notnull\", dflt_value, pk "
                 "from pragma_table_info('" + table + "')")) {
      ColumnMeta c;
      c.name = rowStr(row, {"name"});
      c.type = rowStr(row, {"type"});
      c.notNull = rowVal(row, {"notnull"}).toInt() != 0;
      const mcp::Json dflt = rowVal(row, {"dflt_value"});
      if (!dflt.isNull() && !dflt.isError()) c.dflt = dflt.toString();
      c.pk = rowVal(row, {"pk"}).toInt() > 0;
      if (!c.name.empty()) cols.push_back(std::move(c));
    }
    return cols;
  }

  std::string sql;
  if (type == "mysql") {
    sql = "select column_name, data_type, is_nullable, column_default "
          "from information_schema.columns where table_schema = database() "
          "and table_name = '" + table + "' order by ordinal_position";
  } else if (type == "postgres") {
    sql = "select column_name, data_type, is_nullable, column_default "
          "from information_schema.columns where table_schema = current_schema() "
          "and table_name = '" + table + "' order by ordinal_position";
  } else {  // dm8
    if (!schema.empty() && validIdentifier(schema)) {
      sql = "select column_name, data_type, nullable, data_default "
            "from all_tab_columns where upper(owner) = upper('" +
            schema + "') and table_name = '" + table + "' order by column_id";
    } else {
      sql = "select column_name, data_type, nullable, data_default "
            "from user_tab_columns where table_name = '" + table +
            "' order by column_id";
    }
  }
  const std::vector<std::string> pks =
      sqlPrimaryKeys(db, type, table, schema);
  for (const auto& row : catalogRows(db, sql)) {
    ColumnMeta c;
    c.name = rowStr(row, {"column_name", "COLUMN_NAME"});
    c.type = rowStr(row, {"data_type", "DATA_TYPE"});
    // MySQL's driver folds these to upper case; postgres/sqlite return
    // lower case, dm8 returns NULLABLE/DATA_DEFAULT (D-04).
    const std::string nullable =
        rowStr(row, {"is_nullable", "IS_NULLABLE", "NULLABLE"});
    c.notNull = (nullable == "NO" || nullable == "N");
    const mcp::Json dflt =
        rowVal(row, {"column_default", "COLUMN_DEFAULT", "DATA_DEFAULT"});
    if (!dflt.isNull() && !dflt.isError()) c.dflt = dflt.toString();
    for (const auto& pk : pks) {
      if (pk == c.name) c.pk = true;
    }
    if (!c.name.empty()) cols.push_back(std::move(c));
  }
  return cols;
}

}  // namespace

// ---- schema metadata API ------------------------------------------------------------

std::vector<std::string> listTables(const std::string& dbName) {
  App& g = app();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return {};
  const std::string type = g.specs[dbName].type;
  std::vector<std::string> tables;
  if (type == "jsonfile") {
    for (const auto& e : jsonfileTables(g.specs[dbName].options)) {
      tables.push_back(e.table);
    }
  } else {
    tables = sqlListTables(db, type, dm8Schema(dbName));
  }
  // Keep the list consistent with what the structured tools can address:
  // system/generated names (dm8's ##HISTOGRAMS_TABLE, $INTERNAL, ...) would
  // only make db_describe_table fail afterwards (D-09).
  tables.erase(std::remove_if(tables.begin(), tables.end(),
                              [](const std::string& t) {
                                return !validIdentifier(t);
                              }),
               tables.end());
  return tables;
}

std::vector<ColumnMeta> describeTable(const std::string& dbName,
                                      const std::string& table) {
  App& g = app();
  const std::string type = g.specs.count(dbName) ? g.specs[dbName].type : "";
  const auto hit = g.schemaCache[dbName].find(table);
  if (hit != g.schemaCache[dbName].end()) {
    // jsonfile tables are schemaless: columns evolve as rows are written
    // (first insert creates the table), so an EMPTY cached column set must
    // not stick - recompute it. SQL backends' empty result is authoritative.
    if (!(type == "jsonfile" && hit->second.empty())) return hit->second;
  }

  std::vector<ColumnMeta> cols;
  std::string err;
  ZORM::Idb* db = g.db(dbName);
  if (db != nullptr && validIdentifier(table)) {
    if (type == "jsonfile") {
      for (const auto& e : jsonfileTables(g.specs[dbName].options)) {
        if (e.table != table) continue;
        std::vector<std::string> names = e.columns;
        mcp::Json rows = mcp::Json::ParseJson(e.rowsText, err);
        if (!rows.isArray()) rows = mcp::Json(nullptr);
        if (names.empty() && rows.isArray()) {
          // Tables created by a bare insert have no columns array in the
          // store - fall back to the union of the rows' keys.
          std::set<std::string> seen;
          size_t scanned = 0;
          for (const auto& row : rows.toVector()) {
            if (scanned++ >= 50) break;
            if (!row.isObject()) continue;
            for (const auto& k : row.getAllKeys().toVector()) {
              if (seen.insert(k.toString()).second) names.push_back(k.toString());
            }
          }
        }
        for (const auto& c : names) {
          ColumnMeta m;
          m.name = c;
          m.type = jsonfileInferType(rows, c);
          // jsonfile rows are keyed by auto-generated "id" (docs §2.1).
          if (c == "id") m.pk = true;
          cols.push_back(std::move(m));
        }
        break;
      }
    } else {
      cols = sqlDescribe(db, type, table, dm8Schema(dbName));
    }
  }
  g.schemaCache[dbName][table] = cols;  // cache misses too (empty = unknown)
  return cols;
}

std::vector<ColumnMeta> cachedColumns(const std::string& dbName,
                                      const std::string& table) {
  App& g = app();
  const auto dit = g.schemaCache.find(dbName);
  if (dit == g.schemaCache.end()) return {};
  const auto tit = dit->second.find(table);
  return tit == dit->second.end() ? std::vector<ColumnMeta>{} : tit->second;
}

void discoverAllSchemas() {
  App& g = app();
  for (const auto& [dbName, spec] : g.specs) {
    for (const auto& t : listTables(dbName)) {
      describeTable(dbName, t);
    }
  }
}

}  // namespace zorm_mcp
