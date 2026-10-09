// zorm_server - exposes a zorm database as MCP tools (stdio transport).
//
// Built ONLY inside the zorm repo (the root CMakeLists configures the
// target), so the library itself stays zorm-free and independently
// publishable. Copy this file as a template for "database as MCP server".
//
// Tools (ONE backend, so no `db` routing param - for the multi-backend
// version with MRTR write confirmation see zorm_analytics_server, and for the
// shipped product see src/zorm_mcp_server). Self-reports to MCP clients as
// `zorm-mini-mcp` so it never collides with the product's `zorm-mcp` name;
// register it in a client under its executable name (zorm_server).
//   db_select  - smart query: {tablename, fields?, lks?, ins?, page?, size?, sort?}
//   db_insert  - insert/upsert ONE row: {tablename, data}
//   db_update  - update ONE row: {tablename, data, where:{"id": value}}
//   db_delete  - delete ONE row: {tablename, where:{"id": value}}
// Writes are NOT confirmation-gated here - this is the minimal template; the
// MRTR `input_required` gate lives in zorm_analytics_server / zorm_mcp_server.
//
// Run: zorm_server <dbType> [optionsJson]
// The 2nd argument is a JSON OPTIONS OBJECT passed to DbBase - NOT a file
// path. A bare path is silently ignored and the backend falls back to its
// default (for sqlite3 that means an anonymous temp database), which looks
// like "my data disappeared".
// e.g. zorm_server sqlite3
//      zorm_server sqlite3 '{"connString":"D:/db/demo.db","parameterized":true}'
//      zorm_server jsonfile '{"connString":"./data/db.json"}'
// dbType: sqlite3 | jsonfile | mysql | postgres | dm8

#include "DbBase.h"
#include "Idb.h"
#include <mcp/mcp.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace mcp;
using namespace mcp::server;

namespace {

std::unique_ptr<ZORM::Idb> g_db;  // single connection; single-threaded dispatch

Json toJson(const ZORM::Json& zj) {
  // zjson is shared by both libraries: reparse the wire text.
  std::string err;
  Json parsed = Json::ParseJson(zj.toString(), err);
  return parsed.isError() ? Json(nullptr) : parsed;
}

ZORM::Json toZorm(const Json& j) { return ZORM::Json(j.toString().c_str()); }

McpResult<Json> textResult(const std::string& text) {
  Json result;
  Json content = Json("[]");
  Json item;
  item.add("type", Json::str("text"));
  item.add("text", Json::str(text));
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  return Ok(std::move(result));
}

// db_select: zorm smart query. Reserved keys must not reach zorm's select():
// every non-reserved param key becomes a COLUMN CONDITION, so leaking
// `tablename` fails with "no such column: tablename", and `fields` is a
// separate argument (inside params it becomes a condition too). zorm's own
// `take()`-style reserved keys (sort/size/page/sum/group/lks/ins/ors/fuzzy)
// are recognized there and are safe to pass through.
McpResult<Json> queryTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  ZORM::Json params;
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string key = k.toString();
    if (key == "tablename" || key == "fields") continue;
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    params.add(key, toZorm(v));
  }
  std::vector<std::string> fields;
  const Json f = member(args, "fields");
  if (f.isArray()) {
    for (const auto& item : f.toVector()) fields.push_back(item.toString());
  }
  return textResult(g_db->select(table, params, fields).toString());
}

McpResult<Json> insertTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  // Idb::create = single-row insert/upsert on the "id" column. Do NOT use
  // insertBatch here: it requires an ARRAY of rows and answers
  // status 301 "Param is wrong" when handed the data object directly.
  return textResult(g_db->create(table, toZorm(member(args, "data"))).toString());
}

McpResult<Json> updateTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  // zorm's update targets ONE row and reads the key at the TOP level of
  // params (buildUpdateSql returns STPARAMERR without it); the client-facing
  // where:{"id": v} shape is flattened here.
  const Json idv = member(member(args, "where"), "id");
  if (!(idv.isString() || idv.isNumber())) {
    return Err(McpErrorCode::InvalidParams,
               "db_update requires where:{\"id\": <value>} (single-row update)");
  }
  ZORM::Json params = toZorm(member(args, "data"));
  params.add("id", toZorm(idv));
  return textResult(g_db->update(table, params).toString());
}

McpResult<Json> deleteTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  // Same top-level "id" contract as update; an id the backend cannot resolve
  // makes zorm reject the delete outright rather than degrade to delete-all.
  const Json idv = member(member(args, "where"), "id");
  if (!(idv.isString() || idv.isNumber())) {
    return Err(McpErrorCode::InvalidParams,
               "db_delete requires where:{\"id\": <value>} (single-row delete)");
  }
  ZORM::Json params;
  params.add("id", toZorm(idv));
  return textResult(g_db->remove(table, params).toString());
}

Json schema(const std::string& json) {
  std::string err;
  Json parsed = Json::ParseJson(json, err);
  return parsed.isError() ? Json(R"({"type":"object"})") : parsed;
}

Json toolDef(const std::string& name, const std::string& description, const std::string& schemaJson) {
  Json def;
  def.add("name", Json::str(name));
  def.add("description", Json::str(description));
  def.add("inputSchema", schema(schemaJson));
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: zorm_server <dbType> [optionsJson]\n"
              << "  dbType: sqlite3 | jsonfile | mysql | postgres | dm8\n"
              << "  optionsJson: JSON object passed to DbBase (NOT a file path)\n"
              << "  e.g. zorm_server sqlite3\n"
              << "       zorm_server sqlite3 '{\"connString\":\"D:/db/demo.db\",\"parameterized\":true}'\n"
              << "       zorm_server jsonfile '{\"connString\":\"./data/db.json\"}'\n";
    return 2;
  }
  // Validate the type HERE: DbBase leaves its `Idb* db` uninitialized for an
  // unknown name, so the first tool call would dereference garbage and kill
  // the server (0xC0000005) instead of reporting a bad argument.
  std::string dbType(argv[1]);
  std::transform(dbType.begin(), dbType.end(), dbType.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  static const char* kDbTypes[] = {"sqlite3", "jsonfile", "mysql", "postgres", "dm8"};
  if (std::find(std::begin(kDbTypes), std::end(kDbTypes), dbType) == std::end(kDbTypes)) {
    std::cerr << "unknown dbType: " << argv[1]
              << " (expected one of: sqlite3 | jsonfile | mysql | postgres | dm8)\n";
    return 2;
  }

  ZORM::Json options(argc > 2 ? argv[2] : "");
  g_db.reset(new ZORM::DbBase(dbType, options));

  Builder b;
  b.name("zorm-mini-mcp").version("0.1.0")
      .instructions(std::string("MCP facade over a ") + dbType + " database (zorm).");

  b.tool(toolDef("db_select", "Smart query over a table.",
                 R"({"type":"object","required":["tablename"],
                     "properties":{"tablename":{"type":"string"},
                                   "fields":{"type":"array","items":{"type":"string"}},
                                   "lks":{"type":"object"},"ins":{"type":"object"},
                                   "page":{"type":"integer","minimum":1},
                                   "size":{"type":"integer","minimum":1,"maximum":500},
                                   "sort":{"type":"array","items":{"type":"string"}}}})"),
         [](Ctx& c) { return queryTool(c); });

  b.tool(toolDef("db_insert", "Insert one row.",
                 R"({"type":"object","required":["tablename","data"],
                     "properties":{"tablename":{"type":"string"},
                                   "data":{"type":"object"}}})"),
         [](Ctx& c) { return insertTool(c); });

  b.tool(toolDef("db_update", "Update rows matching where.",
                 R"({"type":"object","required":["tablename","data","where"],
                     "properties":{"tablename":{"type":"string"},
                                   "data":{"type":"object"},
                                   "where":{"type":"object"}}})"),
         [](Ctx& c) { return updateTool(c); });

  b.tool(toolDef("db_delete", "Delete rows matching where.",
                 R"({"type":"object","required":["tablename","where"],
                     "properties":{"tablename":{"type":"string"},
                                   "where":{"type":"object"}}})"),
         [](Ctx& c) { return deleteTool(c); });

  b.acceptLegacyClients(true);  // Claude Code / VS Code speak legacy initialize
  b.supports(ProtocolVersion::V2024_11_05)
      .supports(ProtocolVersion::V2025_03_26)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::V2025_11_25)
      .supports(ProtocolVersion::Latest);

  auto server = b.build();
  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  if (!r.has_value()) {
    std::cerr << "[zorm-mini-mcp] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}
