// resources.cpp - MCP resources (schema self-description) and completion.
//
//   db://guide                   full usage manual (pull channel, see guide.hpp)
//   db://schema                  all backends overview
//   db://schema/{db}             table list of one backend
//   db://schema/{db}/{table}     full column metadata (type/nullable/pk)
//
// The two parametric forms are registered as URI templates (resources/
// templates/list), so every table - including one created at runtime - is
// readable without a restart; the handler discovers tables live. Write tools
// additionally ping subscribers with notifyResourceUpdated.

#include "app.hpp"
#include "guide.hpp"
#include "json_util.hpp"

#include <string>
#include <vector>

namespace zorm_mcp {

using mcp::Json;
using mcp::McpErrorCode;
using mcp::McpResult;
using mcp::member;
using mcp::server::Builder;
using mcp::server::Ctx;

namespace {

// Renders ColumnMeta rows as a compact readable text block.
std::string columnsText(const std::vector<ColumnMeta>& cols) {
  std::string text = "columns:\n";
  if (cols.empty()) text += "  (no columns discovered)\n";
  for (const auto& c : cols) {
    text += "  - " + c.name;
    if (!c.type.empty()) text += " : " + c.type;
    if (c.pk) text += "  [primary key]";
    if (c.notNull && !c.pk) text += "  [not null]";
    if (!c.dflt.empty()) text += "  [default: " + c.dflt + "]";
    text += "\n";
  }
  return text;
}

}  // namespace

McpResult<Json> resourceSchema(Ctx& c) {
  // Resource handlers receive the raw request params (uri at the top level,
  // NOT under "arguments" like tools).
  const Json uriJson = member(c.params, "uri");
  if (!uriJson.isString()) {
    return mcp::Err(McpErrorCode::InvalidParams, "resources/read requires a string uri");
  }
  const std::string uri = uriJson.toString();
  static const std::string kPrefix = "db://schema";
  if (uri.rfind(kPrefix, 0) != 0) {
    return mcp::Err(McpErrorCode::InvalidParams, "unknown resource: " + uri);
  }

  Json result;
  Json contents = Json("[]");
  Json item;
  item.add("uri", Json::str(uri));

  // uri forms: db://schema, db://schema/{db}, db://schema/{db}/{table}
  std::string rest = uri.substr(kPrefix.size());
  if (rest.empty()) {
    std::string text;
    for (const auto& [dbName, spec] : app().specs) {
      text += dbName + " (" + spec.type + "):";
      for (const auto& t : listTables(dbName)) text += " " + t;
      text += "\n";
    }
    item.add("text", Json::str(text));
  } else {
    if (rest[0] == '/') rest.erase(0, 1);
    const size_t slash = rest.find('/');
    const std::string dbName = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string table = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (app().db(dbName) == nullptr) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "unknown db in uri: " + uri + " (available: " +
                          app().dbListText() + ")");
    }
    if (table.empty()) {
      std::string text;
      for (const auto& t : listTables(dbName)) text += t + "\n";
      item.add("text", Json::str(text));
    } else {
      // Unknown table reports distinctly from a real empty-table hit.
      bool known = false;
      for (const auto& t : listTables(dbName)) {
        if (t == table) { known = true; break; }
      }
      if (!known) {
        return mcp::Err(McpErrorCode::InvalidParams, "unknown table: " + table);
      }
      item.add("text", Json::str("table " + table + "\n" +
                                 columnsText(describeTable(dbName, table))));
    }
  }
  contents.push_back(std::move(item));
  result.add("contents", std::move(contents));
  return mcp::Ok(std::move(result));
}

// ---- completion -----------------------------------------------------------------

McpResult<Json> toolCompletion(Ctx& c) {
  // params: {ref: {type, name?, uri?, arguments?...}, argument: {name, value}}
  //
  // The MCP spec's `ref` for a resource template carries `uri` (e.g.
  // "db://schema/{db}") and NO `name`; prompt refs carry `name`. The argument
  // being completed always comes from `argument.name`. The old code read only
  // ref.name, so a spec-shaped client got {"values":[]} - an empty completion.
  const Json ref = member(c.params, "ref");
  const Json argument = member(c.params, "argument");
  const Json refArgs = member(ref, "arguments");
  auto asString = [](const Json& j) {
    return j.isString() ? j.toString() : std::string();
  };
  const std::string uri = asString(member(ref, "uri"));
  const std::string partial = asString(member(argument, "value"));

  std::string argName = asString(member(argument, "name"));
  if (argName.empty()) argName = asString(member(ref, "name"));
  if (argName.empty()) {
    // Derive it from the URI template when the client omits both.
    if (uri.find("{table}") != std::string::npos)
      argName = "table";
    else if (uri.find("{db}") != std::string::npos)
      argName = "db";
  }

  // db from ref.arguments, else from a partially-resolved template path
  // ("db://schema/{db}" has no concrete db, but "db://schema/sqlite3/{table}"
  // does).
  std::string dbName = asString(member(refArgs, "db"));
  if (dbName.empty() && uri.rfind("db://schema/", 0) == 0) {
    std::string seg = uri.substr(std::string("db://schema/").size());
    const size_t slash = seg.find('/');
    if (slash != std::string::npos) seg = seg.substr(0, slash);
    if (!seg.empty() && seg[0] != '{' && app().db(seg) != nullptr) dbName = seg;
  }

  Json result;
  Json values = Json("[]");
  if (argName == "db") {
    for (const auto& [name, spec] : app().specs) {
      if (partial.empty() || name.rfind(partial, 0) == 0)
        values.push_back(Json::str(name));
    }
  } else if (argName == "table") {
    std::vector<std::string> tables;
    if (app().db(dbName) != nullptr) {
      tables = listTables(dbName);
    } else {
      for (const auto& [name, spec] : app().specs) {
        for (const auto& t : listTables(name)) tables.push_back(t);
      }
    }
    for (const auto& t : tables) {
      if (partial.empty() || t.rfind(partial, 0) == 0) values.push_back(Json::str(t));
    }
  } else if (argName == "fields") {
    // fields is an array; argument.value is the entry being typed.
    const std::string table = asString(member(refArgs, "table"));
    for (const auto& col : describeTable(dbName, table)) {
      if (partial.empty() || col.name.rfind(partial, 0) == 0)
        values.push_back(Json::str(col.name));
    }
  }
  result.add("values", std::move(values));
  return mcp::Ok(std::move(result));
}

// ---- registration -----------------------------------------------------------------

void registerResourcesAndCompletion(Builder& b) {
  b.resource(Json(R"({"uri":"db://guide","name":"zorm-mcp usage manual - playbook"})"),
             resourceGuide);
  b.resource(Json(R"({"uri":"db://schema","name":"all backends overview"})"),
             resourceSchema);
  // Parametric URIs are URI templates, not per-table registrations: the table
  // set is discovered live by the handler, so a table created at runtime is
  // readable immediately (no restart, no stale startup snapshot).
  b.resourceTemplate(
      Json(R"({"uriTemplate":"db://schema/{db}","name":"tables in one backend"})"),
      resourceSchema);
  b.resourceTemplate(
      Json(R"({"uriTemplate":"db://schema/{db}/{table}","name":"schema of one table"})"),
      resourceSchema);
  b.completions(toolCompletion);
}

}  // namespace zorm_mcp
