// data_tools.cpp - the data plane:
//   db_query / db_count                    (read; zorm smart query semantics)
//   db_insert / db_insert_batch / db_update / db_delete   (writes, MRTR-gated)
// plus the shared MRTR staging/execute machinery reused by sql_tools.cpp.

#include "app.hpp"
#include "json_util.hpp"
#include "tools.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zorm_mcp {

using mcp::Json;
using mcp::McpErrorCode;
using mcp::McpResult;
using mcp::member;
using mcp::server::Ctx;

namespace {

McpResult<Json> unknownDb(const std::string& dbName) {
  return mcp::Err(McpErrorCode::InvalidParams,
                  "unknown db: " + dbName + " (available: " + app().dbListText() +
                      ") - `db` is the backend NAME, see the server instructions");
}

// ---- structured-identifier validation --------------------------------------------
//
// zorm inlines table/column/alias/sort names into SQL and its default
// quoteIdent() is a no-op on sqlite/mysql/postgres. Every structured
// identifier therefore has to be validated HERE; raw SQL tools
// (db_query_sql / db_exec_sql / db_trans text elements) are explicit escape
// hatches and are intentionally not identifier-checked.

std::string trimCopy(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> splitCsv(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(',', start);
    out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos
                                                           : pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

std::optional<std::string> badIdent(const std::string& what,
                                    const std::string& value) {
  if (validIdentifier(value)) return std::nullopt;
  return "invalid " + what + " name: '" + value +
         "' (letters/digits/underscore, not starting with a digit; complex "
         "names belong in db_query_sql)";
}

// The one wording for "db_delete cannot express this WHERE", used both when
// staging (reject early, never show a doomed confirmation) and when executing.
const char* const kDeleteWhereHint =
    "db_delete supports only where:{\"id\": <value>} (single row) or an empty "
    "where:{} (delete ALL rows). For condition-based deletes stage them in "
    "db_trans (structured delete elements), or query ids with db_query and "
    "delete them one by one";

std::optional<std::string> checkObjectKeys(const Json& obj,
                                           const std::string& what) {
  if (!obj.isObject()) return std::nullopt;
  for (const auto& k : obj.getAllKeys().toVector()) {
    if (auto e = badIdent(what, k.toString())) return e;
  }
  return std::nullopt;
}

// Validates the friendly shapes accepted for sum/count:
//   {"col":"alias"}; ["col"] or ["col,alias"]; "col,alias" or "c1,a1,c2,a2"
std::optional<std::string> checkAggSpec(const Json& v,
                                        const std::string& key) {
  if (v.isObject()) {
    for (const auto& k : v.getAllKeys().toVector()) {
      const std::string col = k.toString();
      if (!(key == "count" && col == "*")) {
        if (auto e = badIdent(key + " column", col)) return e;
      }
      const std::string alias = v[col].toString();
      if (!alias.empty()) {
        if (auto e = badIdent(key + " alias", alias)) return e;
      }
    }
    return std::nullopt;
  }
  if (v.isArray()) {
    for (const auto& item : v.toVector()) {
      const auto parts = splitCsv(item.toString());
      if (parts.size() < 1 || parts.size() > 2)
        return key + " entries must be 'col' or 'col,alias'";
      for (const auto& p : parts) {
        const std::string token = trimCopy(p);
        if (key == "count" && token == "*") continue;
        if (auto e = badIdent(key + " column/alias", token)) return e;
      }
    }
    return std::nullopt;
  }
  if (v.isString()) {
    const auto parts = splitCsv(v.toString());
    // "col" is accepted as shorthand for "col,col" (alias = column); the
    // longer form is always col,alias[,col,alias...].
    if (parts.size() > 1 && parts.size() % 2 != 0)
      return key + " string form must be 'col' or 'col,alias[,col,alias...]'";
    for (const auto& p : parts) {
      const std::string token = trimCopy(p);
      if (key == "count" && token == "*") continue;  // count(*) is legitimate
      if (auto e = badIdent(key + " column/alias", token)) return e;
    }
    return std::nullopt;
  }
  return key + " must be an object, an array, or a string";
}

// Aliases produced by sum/count, used to normalize dm8's string-typed
// aggregate values back to JSON numbers (D-12).
std::vector<std::string> aggregateAliases(const Json& args) {
  std::vector<std::string> aliases;
  for (const char* key : {"sum", "count"}) {
    const Json v = member(args, key);
    if (!v.isObject() && !v.isArray() && !v.isString()) continue;
    if (v.isObject()) {
      for (const auto& k : v.getAllKeys().toVector()) {
        const std::string col = k.toString();
        const std::string alias = v[col].toString();
        aliases.push_back(alias.empty() ? col : alias);
      }
    } else if (v.isArray()) {
      for (const auto& item : v.toVector()) {
        const auto parts = splitCsv(item.toString());
        aliases.push_back(trimCopy(parts.size() > 1 ? parts[1] : parts[0]));
      }
    } else {
      const auto parts = splitCsv(v.toString());
      if (parts.size() == 1) aliases.push_back(trimCopy(parts[0]));
      for (size_t i = 1; i < parts.size(); i += 2)
        aliases.push_back(trimCopy(parts[i]));
    }
  }
  return aliases;
}

// lks/ins/ors travel to zorm as the pair string "col,val,col,val": a comma
// INSIDE a value cannot be represented, and zorm's reply ("Param is wrong.
// details, ors is wrong.") does not say why. Reject early with the fix.
std::optional<std::string> checkPairValue(const std::string& key,
                                          const std::string& value) {
  if (value.find(',') == std::string::npos) return std::nullopt;
  return key + " value \"" + value + "\" contains a comma. The " + key +
         " pair encoding is \"col,value,col,value\", so commas cannot appear "
         "inside a value (this includes compare prefixes like \">,35\"). Use "
         "db_query_sql with an explicit WHERE instead, e.g. {\"sql\":\"select * "
         "from t where price > 35\"}";
}

std::optional<std::string> checkPairValues(const Json& v,
                                           const std::string& key) {
  for (const auto& k : v.getAllKeys().toVector()) {
    const Json vv = v[k.toString()];
    if (vv.isString()) {
      if (auto e = checkPairValue(key, vv.toString())) return e;
    } else if (vv.isArray()) {
      for (const auto& item : vv.toVector()) {
        if (auto e = checkPairValue(key, item.toString())) return e;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> checkSortSpec(const Json& v) {
  if (v.isString()) {
    if (!validSortClause(v.toString()))
      return "invalid sort: '" + v.toString() +
             "' (expected 'col [asc|desc], col2 ...')";
    return std::nullopt;
  }
  if (v.isArray()) {
    for (const auto& item : v.toVector()) {
      const std::string s = item.toString();
      if (!validSortClause(s))
        return "invalid sort: '" + s + "' (expected 'col [asc|desc]')";
    }
    return std::nullopt;
  }
  return "sort must be a string or an array of strings";
}

// Reserved db_query keys (not column conditions).
bool isReservedQueryKey(const std::string& k) {
  static const char* kReserved[] = {
      "db",     "table",  "fields", "lks",   "ins",    "ors",   "group",
      "sort",   "sum",    "count",  "fuzzy", "page",   "size",  "requestState",
      "inputResponses", "approved", "constraint", "rows"};
  return std::any_of(std::begin(kReserved), std::end(kReserved),
                     [&](const char* r) { return k == r; });
}

std::optional<std::string> checkQueryArgs(const Json& args) {
  const Json fields = member(args, "fields");
  if (fields.isArray()) {
    for (const auto& f : fields.toVector()) {
      const std::string name = f.toString();
      if (auto e = badIdent("field", name)) return e;
    }
  } else if (!fields.isNull() && !fields.isError()) {
    return "fields must be an array of column names";
  }

  const Json group = member(args, "group");
  if (group.isArray()) {
    for (const auto& g : group.toVector()) {
      if (auto e = badIdent("group column", g.toString())) return e;
    }
  } else if (group.isString()) {
    if (!validIdentifierList(group.toString()))
      return "invalid group: '" + group.toString() + "'";
  } else if (!group.isNull() && !group.isError()) {
    return "group must be an array or a comma-separated string";
  }

  const Json sort = member(args, "sort");
  if (!sort.isNull() && !sort.isError()) {
    if (auto e = checkSortSpec(sort)) return e;
  }

  for (const char* key : {"lks", "ins", "ors"}) {
    const Json v = member(args, key);
    if (v.isNull() || v.isError()) continue;
    if (!v.isObject())
      return std::string(key) + " must be an object {column: value|values}";
    if (auto e = checkObjectKeys(v, std::string(key) + " column")) return e;
    if (auto e = checkPairValues(v, key)) return e;
  }
  for (const char* key : {"sum", "count"}) {
    const Json v = member(args, key);
    if (v.isNull() || v.isError()) continue;
    if (auto e = checkAggSpec(v, key)) return e;
  }
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string key = k.toString();
    if (isReservedQueryKey(key)) continue;
    if (auto e = badIdent("condition column", key)) return e;
  }
  return std::nullopt;
}

// Validates a structured db_trans element (raw text/sql elements are skipped:
// they ARE the escape hatch).
std::optional<std::string> checkTransElements(const Json& sqls) {
  if (!sqls.isArray()) return "db_trans requires `sqls` to be an array";
  for (const auto& item : sqls.toVector()) {
    if (!item.isObject()) return "db_trans elements must be objects";
    if (ZJSON::hasChild(item, "text") || ZJSON::hasChild(item, "sql")) continue;
    const std::string table = member(item, "table").toString();
    if (auto e = badIdent("transaction table", table)) return e;
    std::string method = member(item, "method").toString();
    for (auto& ch : method)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (method != "insert" && method != "update" && method != "delete" &&
        method != "batch")
      return "db_trans structured method must be insert|update|delete|batch";
    const Json params = member(item, "params");
    if (!params.isNull() && !params.isError()) {
      if (!params.isObject()) return "db_trans element params must be an object";
      if (auto e = checkObjectKeys(params, "transaction column")) return e;
    }
  }
  return std::nullopt;
}

// Validates table + payload identifiers for a write kind. Called both when
// staging (so the confirmation text never shows an injected target) and when
// executing (defense in depth).
std::optional<std::string> checkWriteArgs(const std::string& kind,
                                          const Json& args) {
  const std::string table = member(args, "table").toString();
  if (kind == "exec_sql") {
    if (member(args, "sql").toString().empty()) return "sql is required";
    return std::nullopt;  // raw SQL escape hatch
  }
  if (kind == "trans") return checkTransElements(member(args, "sqls"));
  if (auto e = badIdent("table", table)) return e;

  if (kind == "insert" || kind == "update") {
    const Json data = member(args, "data");
    if (!data.isObject()) return kind + " requires `data` to be an object";
    if (auto e = checkObjectKeys(data, "column")) return e;
    if (kind == "update") {
      const Json where = member(args, "where");
      if (!where.isObject() || !ZJSON::hasChild(where, "id"))
        return "db_update requires where:{\"id\": <value>} (single-row update) - "
               "for condition-based updates use db_trans with a structured "
               "update element (or db_exec_sql with --allow-sql)";
    }
  } else if (kind == "insert_batch") {
    const Json rows = member(args, "rows");
    if (!rows.isArray() || rows.size() < 1)
      return "db_insert_batch requires `rows` to be a non-empty array";
    for (const auto& row : rows.toVector()) {
      if (!row.isObject()) return "each row must be an object";
      if (auto e = checkObjectKeys(row, "column")) return e;
    }
    const Json constraint = member(args, "constraint");
    if (constraint.isString() && !constraint.toString().empty()) {
      if (auto e = badIdent("constraint", constraint.toString())) return e;
    }
  } else if (kind == "delete") {
    const Json where = member(args, "where");
    if (!where.isObject()) return "db_delete requires `where` to be an object";
    for (const auto& k : where.getAllKeys().toVector()) {
      if (auto e = badIdent("where column", k.toString())) return e;
    }
    // Reject a non-id condition HERE, before staging. Staging first showed
    // the user a confirmation (with a row-count estimate) for an operation
    // that could only fail after they approved it.
    const Json idv = member(where, "id");
    const bool hasId = idv.isString() || idv.isNumber();
    if (!hasId && !ZJSON::isEmptyObject(where)) return kDeleteWhereHint;
  }
  return std::nullopt;
}

McpResult<Json> invalidArgs(const std::string& message) {
  return mcp::Err(McpErrorCode::InvalidParams, message);
}

// ---- db_query argument conversion -------------------------------------------------
//
// zorm's smart-query shapes (verified against SqlBackendBase::genSql):
//   lks/ins/ors : COMMA-SEPARATED STRING "col1,val1,col2,val2" (pairs)
//   sum/count   : comma-separated "col,alias" pairs
//   group/sort  : comma-separated "a,b"
//   page/size/fuzzy : scalars
// The client-facing schema uses friendlier shapes (objects / arrays); convert
// here. Values are assumed simple (no embedded commas) - documented in the
// playbook.

void convertPairsObject(const Json& v, std::string& joined,
                        bool sameKeyForArray) {
  for (const auto& k : v.getAllKeys().toVector()) {
    const std::string kk = k.toString();
    const Json vv = v[kk];
    if (vv.isString() || vv.isNumber() || vv.isTrue() || vv.isFalse()) {
      if (!joined.empty()) joined += ",";
      joined += kk + "," + vv.toString();
    } else if (vv.isArray()) {
      if (sameKeyForArray) {
        // ins: {age:[18,20]} -> "age,18,20" (ONE column, N values).
        if (!joined.empty()) joined += ",";
        joined += kk;
        for (const auto& item : vv.toVector()) joined += "," + item.toString();
      } else {
        // lks/ors: every value is a separate condition -> "col,v,col,v".
        for (const auto& item : vv.toVector()) {
          if (!joined.empty()) joined += ",";
          joined += kk + "," + item.toString();
        }
      }
    }
    // object values are not supported by the pair encoding
  }
}

void convertStringLists(const Json& args, ZORM::Json& params, bool stripWildcard) {
  // lks/ins/ors: object {col:value} -> "col1,val1,col2,val2"
  for (const char* key : {"lks", "ins", "ors"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isObject()) {
      std::string joined;
      if (stripWildcard && std::string(key) == "lks") {
        // jsonfile's lks is a plain substring match - the portable
        // %wildcard semantics have no backend equivalent, so strip the
        // wildcards and let "contains" approximate them.
        Json stripped = Json("{}");
        for (const auto& k : v.getAllKeys().toVector()) {
          std::string val = v[k.toString()].toString();
          val.erase(std::remove(val.begin(), val.end(), '%'), val.end());
          stripped.add(k.toString(), Json::str(val));
        }
        convertPairsObject(stripped, joined, false);
      } else {
        convertPairsObject(v, joined, std::string(key) == "ins");
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString() || v.isArray()) {
      params.add(key, toZormJson(v));  // pass through zorm-native shapes
    }
  }
  // group/sort: array ["a","b"] -> "a,b"
  for (const char* key : {"group", "sort"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        if (!joined.empty()) joined += ",";
        joined += item.toString();
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString()) {
      params.add(key, toZormJson(v));
    }
  }
  // sum/count: zorm consumes "col,alias[,col,alias...]" pairs.
  //   object {col:alias}  -> col,alias (alias defaults to the column)
  //   array  ["col"]      -> col,col   (["col,alias"] passes through)
  //   string "col,alias"  -> unchanged (validated upstream)
  // `count:"*"` / `count:["*"]` / `count:{"*":""}` mean count(*) - but using
  // "*" as the DEFAULT ALIAS produced "count(*) as *" (a syntax error on every
  // backend). The anonymous form aliases to `total` instead; an explicit alias
  // ("*,n", {"*":"n"}) is untouched.
  auto aggAlias = [](const std::string& col) -> std::string {
    return col == "*" ? "total" : col;
  };
  for (const char* key : {"sum", "count"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isObject()) {
      std::string joined;
      for (const auto& k : v.getAllKeys().toVector()) {
        const std::string kk = k.toString();
        const std::string alias = v[kk].toString();
        if (!joined.empty()) joined += ",";
        joined += kk + "," + (alias.empty() ? aggAlias(kk) : alias);
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        const std::string s = item.toString();
        if (s.empty()) continue;
        if (!joined.empty()) joined += ",";
        joined += s.find(',') == std::string::npos ? s + "," + aggAlias(s) : s;
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString()) {
      const std::string s = v.toString();
      if (!s.empty())
        params.add(key, ZORM::Json(
                            (s.find(',') == std::string::npos ? s + "," + aggAlias(s) : s)
                                .c_str()));
    }
  }
  // scalars
  for (const char* key : {"fuzzy", "page", "size"}) {
    const Json v = member(args, key);
    if (!v.isError() && !v.isNull()) params.add(key, toZormJson(v));
  }
}

// ---- MRTR machinery (shared with sql_tools.cpp) -------------------------------------

// Extracts the user's approval from an inputResponses payload. Clients echo
// elicitation answers in different shapes; accept them all:
//   {"confirm":{"approved":true}}   canonical (inputRequests key)
//   {"approved":true}               flat
//   {"content"/"value"/"params":{"approved":true}}
//   {"action":"accept"}             (or decline/reject/cancel)
// Anything unrecognized counts as NOT approved (fail-safe).
bool confirmApproved(const Json& responses) {
  static const char* kNested[] = {"confirm", "content", "value", "params"};
  for (const char* key : kNested) {
    const Json nested = member(responses, key);
    if (nested.isObject()) {
      const Json approved = member(nested, "approved");
      if (approved.isTrue() || approved.isFalse()) return approved.isTrue();
    }
  }
  const Json flat = member(responses, "approved");
  if (flat.isTrue() || flat.isFalse()) return flat.isTrue();
  const Json action = member(responses, "action");
  if (action.isString()) {
    const std::string a = action.toString();
    return a == "accept" || a == "accepted" || a == "approve" || a == "approved";
  }
  return false;
}

// Human phrase for the affected rows of a staged delete (count-first so the
// confirmation text states the impact). zorm's count aggregate lands in
// data[0]["total"] - data.size() is always 1 and must never be used here.
std::string deleteImpact(ZORM::Idb* db, const std::string& table,
                         const Json& where) {
  ZORM::Json wp;
  if (ZJSON::hasChild(where, "id")) {
    wp.add("id", toZormJson(where["id"]));
  } else {
    // top-level conditions (zorm select semantics: conditions live in params)
    for (const auto& k : where.getAllKeys().toVector()) {
      wp.add(k.toString(), toZormJson(where[k.toString()]));
    }
  }
  wp.add("count", ZORM::Json("*,total"));
  const ZORM::Json rs = db->select(table, wp);
  if (okStatus(rs) && rs["data"].isArray() && rs["data"].size() > 0) {
    const long long total = rs["data"][0]["total"].toInt64();
    return std::to_string(total) + (total == 1 ? " row" : " rows");
  }
  return "unknown number of rows";
}

}  // namespace

// Builds the input_required result asking the user to confirm a write.
Json inputRequiredConfirm(const std::string& requestState, const std::string& message) {
  Json result;
  result.add(std::string(mcp::kResultTypeKey),
             Json::str(std::string(mcp::kResultTypeInputRequired)));
  // CallToolResult requires content even for input_required results.
  Json content = Json("[]");
  Json text;
  text.add("type", Json::str("text"));
  text.add("text", Json::str(message));
  content.push_back(std::move(text));
  result.add("content", std::move(content));
  result.add("requestState", Json::str(requestState));
  Json inputRequests;
  Json confirm;
  confirm.add("method", Json::str("elicitation/create"));
  Json params;
  params.add("mode", Json::str("form"));
  params.add("message", Json::str(message));
  Json schema;
  schema.add("type", Json::str("object"));
  Json props;
  Json approved;
  approved.add("type", Json::str("boolean"));
  props.add("approved", approved);
  schema.add("properties", std::move(props));
  schema.add("required", Json(R"(["approved"])"));
  params.add("requestedSchema", std::move(schema));
  confirm.add("params", std::move(params));
  inputRequests.add("confirm", std::move(confirm));
  result.add("inputRequests", std::move(inputRequests));
  return result;
}

// Executes an already-staged write (MRTR-approved retry / --yes fast path).
McpResult<Json> executeWrite(const std::string& dbName, const std::string& table,
                             const std::string& kind, const Json& args) {
  App& g = app();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  if (auto e = checkWriteArgs(kind, args)) return invalidArgs(*e);
  const DbSpec& spec = g.specs[dbName];

  ZORM::Json rs;
  if (kind == "insert") {
    // Idb::create semantics: single-row insert; a missing id is
    // auto-generated, an existing id upserts (covers jsonfile parity).
    rs = db->create(table, toZormJson(member(args, "data")));
  } else if (kind == "insert_batch") {
    // Idb::insertBatch: multi-row upsert on the constraint column.
    const Json rows = member(args, "rows");
    if (!rows.isArray() || rows.size() < 1) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_insert_batch requires `rows` to be a non-empty array of row objects");
    }
    // zjson trap: member() of a missing key yields Null whose toString() is
    // the literal "null" - test presence explicitly, never via toString().
    std::string constraint = "id";
    const Json c = member(args, "constraint");
    if (c.isString() && !c.toString().empty()) constraint = c.toString();
    rs = db->insertBatch(table, toZormJson(rows), constraint);
  } else if (kind == "update") {
    // zorm update: the target id rides at the TOP level of params
    // (buildUpdateSql reads params["id"]) plus the columns to change.
    // Client shape {data:{cols}, where:{id}} is flattened here.
    const Json idv = member(member(args, "where"), "id");
    if (!(idv.isString() || idv.isNumber())) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_update requires where:{\"id\": <value>} (single-row update). "
                      "For condition-based updates use db_trans with a structured "
                      "update element, or db_exec_sql when --allow-sql is on");
    }
    ZORM::Json params = toZormJson(member(args, "data"));
    params.add("id", toZormJson(idv));
    rs = db->update(table, params);
  } else if (kind == "delete") {
    // zorm remove is id-targeted (params["id"] at the top level). where:{}
    // means "delete ALL rows" - structured remove cannot express it, so fall
    // back to execSql with a VALIDATED table name (identifier whitelist keeps
    // client-supplied names out of SQL). A non-empty where without id is
    // rejected with guidance, never treated as delete-all.
    // NOTE: member() returns Json(nullptr) for a missing key and zjson's
    // toString() renders Null as "null" - presence is tested via hasChild.
    const Json where = member(args, "where");
    const Json idv = member(where, "id");
    if (idv.isString() || idv.isNumber()) {
      ZORM::Json params;
      params.add("id", toZormJson(idv));
      rs = db->remove(table, params);
    } else if (ZJSON::isEmptyObject(where)) {
      if (!validIdentifier(table)) {
        return mcp::Err(McpErrorCode::InvalidParams, "invalid table name: " + table);
      }
      if (g.specs[dbName].type == "jsonfile") {
        // jsonfile's SQL shim only supports DELETE ... WHERE id = ?. Delete-all
        // goes through the structured interface instead: select the ids, then
        // remove row by row (adapter-level; Idb stays untouched).
        const ZORM::Json rows = db->select(table, ZORM::Json());
        if (!okStatus(rows)) {
          rs = rows;
        } else {
          long long n = 0;
          bool allOk = true;
          std::string lastErr;
          if (rows["data"].isArray()) {
            for (const auto& row : rows["data"].toVector()) {
              if (!ZJSON::hasChild(row, "id")) continue;
              ZORM::Json p;
              p.add("id", row["id"]);
              const ZORM::Json one = db->remove(table, p);
              if (!okStatus(one)) {
                allOk = false;
                lastErr = one["message"].toString();
                break;
              }
              ++n;
            }
          }
          ZORM::Json out(R"({"status":200})");
          out.add("message", allOk ? Json::str(std::string("delete all ok"))
                                   : Json::str("delete all aborted: " + lastErr));
          out.add("affectedRows", Json(static_cast<int64_t>(n)));
          rs = out;
        }
      } else {
        // dm8 needs the schema-qualified quoted name (unquoted identifiers
        // fold to upper case there and miss zorm's quoted-lowercase DDL).
        std::string target = table;
        if (spec.type == "dm8") {
          std::string schema;
          if (ZJSON::hasChild(spec.options, "db_name"))
            schema = spec.options["db_name"].toString();
          target = schema.empty() ? "\"" + table + "\""
                                  : "\"" + schema + "\".\"" + table + "\"";
        }
        rs = db->execSql("delete from " + target);
      }
    } else {
      return mcp::Err(McpErrorCode::InvalidParams, kDeleteWhereHint);
    }
  } else if (kind == "exec_sql") {
    std::string sql = member(args, "sql").toString();
    const Json values = member(args, "values");
    ZORM::Json zvals(R"([])");
    if (values.isArray()) zvals = toZormJson(values);
    if (spec.type == "postgres" && zvals.size() > 0) {
      std::string rewritten = sql;
      const size_t n = rewritePgPlaceholders(rewritten);
      if (n > 0) {
        if (n != static_cast<size_t>(zvals.size())) {
          return mcp::Err(McpErrorCode::InvalidParams,
                          "SQL has " + std::to_string(n) +
                              " '?' placeholders but `values` has " +
                              std::to_string(zvals.size()) +
                              " entries (values must match placeholders 1:1)");
        }
        sql = rewritten;
      }
    }
    rs = db->execSql(sql, ZORM::Json(), zvals);
  } else if (kind == "trans") {
    const Json sqls = member(args, "sqls");
    if (!sqls.isArray() || sqls.size() < 1) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_trans requires `sqls`: a non-empty array of transaction elements");
    }
    // zorm's structured elements use capitalized methods ("Insert" /
    // "Update" / "Delete" / "Batch"); the client-facing schema documents
    // lowercase - normalize here so agents are never punished for case.
    ZORM::Json zsqls(R"([])");
    for (const auto& item : sqls.toVector()) {
      ZORM::Json el = toZormJson(item);
      // PostgreSQL uses $n placeholders. Normalize `?` in raw text elements
      // so one SQL shape works on every backend.
      if (spec.type == "postgres" &&
          (ZJSON::hasChild(el, "text") || ZJSON::hasChild(el, "sql"))) {
        const char* key = ZJSON::hasChild(el, "text") ? "text" : "sql";
        std::string text = el[key].toString();
        size_t valueCount = 0;
        if (ZJSON::hasChild(el, "values") && el["values"].isArray())
          valueCount = static_cast<size_t>(el["values"].size());
        const size_t n = rewritePgPlaceholders(text);
        if (n > 0) {
          if (n != valueCount) {
            return mcp::Err(McpErrorCode::InvalidParams,
                            "transaction element has " + std::to_string(n) +
                                " '?' placeholders but " +
                                std::to_string(valueCount) +
                                " values (must match 1:1)");
          }
          ZJSON::setChild(el, key, ZORM::Json::str(text));
        }
      }
      if (ZJSON::hasChild(el, "method")) {
        std::string m = el["method"].toString();
        for (auto& ch : m)
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string mapped;
        if (m == "insert") mapped = "Insert";
        else if (m == "update") mapped = "Update";
        else if (m == "delete") mapped = "Delete";
        else if (m == "batch") mapped = "Batch";
        if (!mapped.empty()) ZJSON::setChild(el, "method", ZORM::Json(mapped.c_str()));
      }
      zsqls.push_back(el);
    }
    rs = db->transGo(zsqls);
  } else {
    return mcp::Err(McpErrorCode::InternalError, "unknown write kind: " + kind);
  }

  if (!okStatus(rs)) {
    // Raw driver codes are opaque to AI clients; map the common sqlite ones
    // and always name the exact file that was being written (connString is
    // relative - the resolved path is what matters when the client-spawned
    // cwd differs from the repo root).
    std::string msg = rs["message"].toString();
    if (spec.type == "dm8") msg = toUtf8FromGbk(msg);
    const std::string raw = "Error Code : ";
    const size_t pos = msg.find(raw);
    if (pos != std::string::npos) {
      const std::string code = msg.substr(pos + raw.size());
      if (code == "8")
        msg += " (SQLITE_READONLY: the database file or its directory is read-only - "
               "check the file attribute, disk, and the server process's working directory)";
      else if (code == "5")
        msg += " (SQLITE_BUSY: the database is locked by another connection/process)";
      else if (code == "11")
        msg += " (SQLITE_CORRUPT: the database file is corrupted)";
      else if (code == "13")
        msg += " (SQLITE_FULL: disk is full or quota exceeded)";
      else if (code == "14")
        msg += " (SQLITE_CANTOPEN: the database file cannot be opened - wrong path or no permission)";
    }
    // Only file-backed backends have a connString. Server backends used to
    // print "[file: <repo root>]" because a missing connString renders as
    // "null" - which misleads the AI and the user (D-12).
    std::string conn;
    if (ZJSON::hasChild(spec.options, "connString"))
      conn = spec.options["connString"].toString();
    if (!conn.empty() && conn != "null") {
      try {
        conn = std::filesystem::absolute(conn).string();
      } catch (...) {
      }
      msg += " [file: " + conn + "]";
    }
    return mcp::Err(McpErrorCode::InternalError, "write failed: " + msg);
  }

  // Notify resource subscribers that this table changed (mcp-cpp fan-out).
  if (auto srv = g.server.lock()) {
    if (!table.empty()) {
      srv->notifyResourceUpdated("db://schema/" + dbName + "/" + table);
    }
    srv->notifyListChanged("resources");
  }
  // Envelope normalization (D-12): expose both affected/affectedRows, and do
  // not report a single insertId for a multi-row batch.
  Json out = toMcpJson(rs);
  if (out.isObject()) {
    if (!ZJSON::hasChild(out, "affected") && ZJSON::hasChild(out, "affectedRows"))
      out.add("affected", out["affectedRows"]);
    if (!ZJSON::hasChild(out, "affectedRows") && ZJSON::hasChild(out, "affected"))
      out.add("affectedRows", out["affected"]);
    if (kind == "insert_batch" && ZJSON::hasChild(out, "insertId"))
      out.remove("insertId");
  }
  return mcp::Ok(toolResult(out));
}

// Stages a write and returns input_required (or executes directly with --yes
// / on an approved retry). Shared by every write tool; `kind` selects the
// executor and the confirmation wording.
McpResult<Json> stageWrite(Ctx& c, const std::string& kind) {
  App& g = app();
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  // Validate BEFORE staging: the confirmation text must never show an injected
  // target, and an approved retry must re-validate as defense in depth.
  if (auto e = checkWriteArgs(kind, args)) return invalidArgs(*e);

  // MRTR retry detection. Per the spec the retry carries `requestState` and
  // `inputResponses` at the tools/call PARAMS level (siblings of `arguments`),
  // which is also what mcp-cpp's Ctx::openRequestState reads. Older callers of
  // this server put them inside `arguments`; that placement is accepted for
  // inputResponses only (it carries no integrity claim).
  const Json rootResponses = member(c.params, "inputResponses");
  const Json rootState = member(c.params, "requestState");
  const Json rootApproved = member(c.params, "approved");
  const Json argResponses = member(args, "inputResponses");
  const Json argState = member(args, "requestState");
  const Json argApproved = member(args, "approved");
  const Json responses = rootResponses.isObject() ? rootResponses : argResponses;
  const Json reqStateArg = rootState.isString() ? rootState : argState;
  const Json approvedArg =
      (rootApproved.isTrue() || rootApproved.isFalse()) ? rootApproved : argApproved;
  if (rootState.isNull() && argState.isString()) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "requestState must be sent at the tools/call params level "
                    "(sibling of `arguments`), not inside `arguments` - echo the "
                    "requestState value from the input_required result verbatim");
  }
  const bool isRetry = responses.isObject() || reqStateArg.isString() ||
                       approvedArg.isTrue() || approvedArg.isFalse();
  if (isRetry) {
    const bool approved =
        responses.isObject() ? confirmApproved(responses) : approvedArg.isTrue();
    // Locate the pending op. An echoed requestState is verified by the
    // mcp-cpp RequestStateCodec: only a token THIS server sealed for THIS
    // tool (and, over HTTP, THIS principal) opens. A forged, expired, or
    // cross-tool token is rejected instead of being used as a map key.
    std::string state;
    PendingOp op;
    bool found = false;
    bool hadState = false;
    if (reqStateArg.isString()) {
      hadState = true;
      const auto opened = c.openRequestState();
      if (!opened.has_value()) {
        return mcp::Err(McpErrorCode::InvalidParams,
                        "requestState is invalid, expired, forged, or was issued "
                        "for a different tool - call again without "
                        "requestState/inputResponses to stage a new one");
      }
      if (opened.value().has_value()) {
        const std::string s = *opened.value();
        const auto it = g.pending.find(s);
        if (it != g.pending.end()) {
          state = s;
          op = it->second;
          found = true;
        }
      }
      if (!found) {
        // Verified state pointing at an op this process no longer holds
        // (already consumed, or the server restarted). Never fall through to
        // the tuple fallback here: that could execute a DIFFERENT staged op.
        return mcp::Err(McpErrorCode::InvalidParams,
                        "requestState refers to no pending " + kind + " op for " +
                            dbName + (table.empty() ? "" : "." + table) +
                            " (already executed or expired) - call again without "
                            "requestState/inputResponses to stage a new one");
      }
    }
    if (!hadState) {
      // Client dropped the state but still answered: fall back to the pending
      // op for THIS (db, table, kind). With several staged (the client looping
      // before confirming), the latest one wins.
      int bestN = -1;
      for (const auto& [s, po] : g.pending) {
        if (po.db != dbName || po.table != table || po.kind != kind) continue;
        int n = 0;
        try {
          n = std::stoi(s.substr(s.find_last_of('-') + 1));
        } catch (...) {
          continue;
        }
        if (n > bestN) {
          bestN = n;
          state = s;
          op = po;
          found = true;
        }
      }
      if (!found) {
        return mcp::Err(McpErrorCode::InvalidParams,
                        "no pending " + kind + " op for " + dbName +
                            (table.empty() ? "" : "." + table) +
                            " - call again without requestState/inputResponses "
                            "to stage a new one");
      }
    }
    g.pending.erase(state);
    if (!approved) {
      Json result;
      result.add(std::string(mcp::kResultTypeKey),
                 Json::str(std::string(mcp::kResultTypeComplete)));
      result.add("content", Json(R"([{"type":"text","text":"Write cancelled by user."}])"));
      return mcp::Ok(std::move(result));
    }
    return executeWrite(op.db, op.table, op.kind, op.args);
  }

  if (g.confirmWrites) {
    const std::string state = "st-" + std::to_string(g.nextState++);
    // Impact estimate for the confirmation text.
    std::string impact;
    if (kind == "delete" && member(args, "where").isObject()) {
      impact = deleteImpact(db, table, member(args, "where"));
    } else if (kind == "insert_batch" && member(args, "rows").isArray()) {
      impact = std::to_string(member(args, "rows").size()) + " rows";
    } else if (kind == "insert") {
      impact = "1 row";
    } else if (kind == "update") {
      impact = "row id=" + member(member(args, "where"), "id").toString();
    } else if (kind == "trans" && member(args, "sqls").isArray()) {
      const int64_t n = member(args, "sqls").size();
      impact = std::to_string(n) + (n == 1 ? " statement" : " statements");
    } else if (kind == "exec_sql") {
      impact = "arbitrary SQL";
    }
    // A new stage for the same (db,table,kind) replaces the old pending -
    // stale confirm requests are meaningless and would pile up.
    for (auto it = g.pending.begin(); it != g.pending.end();) {
      if (it->second.db == dbName && it->second.table == table && it->second.kind == kind)
        it = g.pending.erase(it);
      else
        ++it;
    }
    g.pending[state] = PendingOp{dbName, table, kind, args};
    // Seal the internal op id into the wire token. The client only ever echoes
    // it; the codec binds it to this tool and (with an HTTP principal) this
    // caller, so it cannot be forged or replayed against another write tool.
    const auto sealed = c.sealRequestState(state);
    if (!sealed.has_value()) {
      g.pending.erase(state);
      return mcp::Err(McpErrorCode::InternalError,
                      "failed to seal requestState: " + sealed.error().message);
    }
    // requestState rides in the confirmation text so strict text-only
    // clients can echo it back verbatim on the retry.
    const std::string msg = "Confirm " + kind + " on " + dbName +
                            (table.empty() ? "" : "." + table) + " (" + impact +
                            ")? Reply with approved:true to proceed (requestState: " +
                            sealed.value() + ").";
    return mcp::Ok(inputRequiredConfirm(sealed.value(), msg));
  }
  // --yes: execute immediately (tests / automation).
  return executeWrite(dbName, table, kind, args);
}

// ---- read handlers -----------------------------------------------------------------

McpResult<Json> toolQuery(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  if (table.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "table is required");
  }
  if (auto e = badIdent("table", table)) return invalidArgs(*e);
  if (auto e = checkQueryArgs(args)) return invalidArgs(*e);

  ZORM::Json params;
  const bool jsonfileBackend = app().specs[dbName].type == "jsonfile";
  convertStringLists(args, params, jsonfileBackend);

  // Pagination normalization (D-10): zorm only paginates when page > 0.
  //   size only -> page 1; page only -> explicit error (limit 0,0 otherwise)
  const Json pageV = member(args, "page");
  const Json sizeV = member(args, "size");
  const bool havePage = pageV.isNumber();
  const bool haveSize = sizeV.isNumber();
  if (havePage != haveSize) {
    if (havePage) {
      return invalidArgs(
          "page requires size: pass both (page starts at 1), e.g. "
          "{\"page\":1,\"size\":20}");
    }
    params.add("page", Json(static_cast<int64_t>(1)));
  }
  if (haveSize) {
    const long long size = sizeV.toInt64();
    if (size < 1 || size > 500)
      return invalidArgs("size must be between 1 and 500");
  }
  // Remaining keys pass through as plain column conditions: {"price":100}
  // equal, or a compare-prefix string {"price":">,100"} (zorm prefixes
  // >, >=, <, <=, <>, =).
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string kk = k.toString();
    static const char* kReserved[] = {"db",       "table",  "fields", "lks",
                                      "ins",      "ors",    "group",  "sort",
                                      "sum",      "count",  "fuzzy",  "page",
                                      "size",     "requestState", "inputResponses",
                                      "approved", "constraint", "rows"};
    if (std::any_of(std::begin(kReserved), std::end(kReserved),
                    [&](const char* r) { return kk == r; })) {
      continue;
    }
    const Json v = member(args, kk);
    if (v.isError() || v.isNull()) continue;
    params.add(kk, toZormJson(v));
  }

  std::vector<std::string> fields;
  const Json f = member(args, "fields");
  if (f.isArray()) {
    for (const auto& item : f.toVector()) fields.push_back(item.toString());
  }

  const ZORM::Json rs = db->select(table, params, fields);
  if (!okStatus(rs)) {
    std::string msg = rs["message"].toString();
    if (app().specs[dbName].type == "dm8") msg = toUtf8FromGbk(msg);
    return mcp::Err(McpErrorCode::InvalidParams, "query failed: " + msg);
  }

  // dm8 returns aggregates as strings; convert the aliases THIS request asked
  // for (never arbitrary columns that merely look numeric).
  Json out = toMcpJson(rs);
  const std::vector<std::string> aliases = aggregateAliases(args);
  const Json data = member(out, "data");
  if (!aliases.empty() && data.isArray() && data.size() > 0) {
    Json arr = Json("[]");
    for (const auto& row : data.toVector()) {
      Json normalized = row;
      for (const auto& alias : aliases) {
        const Json v = member(normalized, alias);
        if (!v.isString()) continue;
        const std::string s = v.toString();
        try {
          size_t used = 0;
          if (s.find_first_of(".eE") == std::string::npos) {
            const long long n = std::stoll(s, &used);
            if (used == s.size())
              ZJSON::setChild(normalized, alias, Json(n));
          } else {
            const double d = std::stod(s, &used);
            if (used == s.size())
              ZJSON::setChild(normalized, alias, Json(d));
          }
        } catch (...) {
        }
      }
      arr.push_back(std::move(normalized));
    }
    ZJSON::setChild(out, "data", arr);
  }
  return mcp::Ok(toolResult(out));
}

McpResult<Json> toolCount(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  if (table.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "table is required");
  }
  if (auto e = badIdent("table", table)) return invalidArgs(*e);

  // No silent drops: a condition passed OUTSIDE `where` would otherwise be
  // ignored and the caller would get the whole-table count. Naming the key
  // and showing the accepted shape is the cheapest correction loop.
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string key = k.toString();
    if (key == "db" || key == "table" || key == "where") continue;
    if (key == "requestState" || key == "inputResponses" || key == "approved")
      continue;  // protocol-level retry fields tolerated by every tool
    return invalidArgs("unknown argument '" + key +
                       "': db_count accepts db, table, where. Put conditions "
                       "inside where, e.g. {\"where\":{\"" +
                       key + "\":<value>}}");
  }

  // zorm count expects a "col,alias" pair (e.g. "*,total"); optional where
  // keys merge into params as top-level conditions (zorm select semantics).
  ZORM::Json params;
  const Json where = member(args, "where");
  if (where.isObject()) {
    if (auto e = checkObjectKeys(where, "where column")) return invalidArgs(*e);
    for (const auto& k : where.getAllKeys().toVector()) {
      const std::string kk = k.toString();
      params.add(kk, toZormJson(where[kk]));
    }
  }
  params.add("count", ZORM::Json("*,total"));
  const ZORM::Json rs = db->select(table, params);
  if (!okStatus(rs)) {
    std::string msg = rs["message"].toString();
    if (app().specs[dbName].type == "dm8") msg = toUtf8FromGbk(msg);
    return mcp::Err(McpErrorCode::InvalidParams, "count failed: " + msg);
  }
  // dm8 reports aggregate values as strings; normalize `total` to a number so
  // clients can compare it without per-dialect special cases (D-12).
  Json out = toMcpJson(rs);
  const Json data = member(out, "data");
  if (data.isArray() && data.size() > 0) {
    Json arr = Json("[]");
    for (const auto& row : data.toVector()) {
      Json normalized = row;
      const Json total = member(normalized, "total");
      if (total.isString()) {
        try {
          ZJSON::setChild(normalized, "total",
                          Json(std::stoll(total.toString())));
        } catch (...) {
        }
      }
      arr.push_back(std::move(normalized));
    }
    ZJSON::setChild(out, "data", arr);
  }
  return mcp::Ok(toolResult(out));
}

McpResult<Json> toolInsert(Ctx& c) { return stageWrite(c, "insert"); }
McpResult<Json> toolInsertBatch(Ctx& c) { return stageWrite(c, "insert_batch"); }
McpResult<Json> toolUpdate(Ctx& c) { return stageWrite(c, "update"); }
McpResult<Json> toolDelete(Ctx& c) { return stageWrite(c, "delete"); }

// ---- registration -------------------------------------------------------------------

void registerDataTools(mcp::server::Builder& b) {
  b.tool(Json(R"({"name":"db_query","description":"PREFERRED data query - structured JSON, no SQL, same semantics on every backend. Supports: fields (projection), lks (fuzzy, % wildcards), ins (IN-list), ors (OR), group+sum (aggregation), sort, page/size (pagination), plus plain column conditions: {\"price\":100} equals, {\"price\":\">,100\"} compares (prefixes >, >=, <, <=, <>, =). Use db_query_sql only when JOINs or dialect functions are unavoidable.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string","description":"backend name"},"table":{"type":"string"},"fields":{"type":"array","items":{"type":"string"},"description":"column projection"},"lks":{"type":"object","description":"fuzzy matches, % wildcards, e.g. {\"name\":\"%abc%\"}; values cannot contain commas"},"ins":{"type":"object","description":"IN-list matches; an array value means multiple values for that column, e.g. {\"age\":[18,20]}; values cannot contain commas"},"ors":{"type":"object","description":"OR conditions; values cannot contain commas - use db_query_sql for compare-prefix values like \">,35\""},"group":{"type":"array","items":{"type":"string"},"description":"GROUP BY columns"},"sum":{"oneOf":[{"type":"object","additionalProperties":{"type":"string"}},{"type":"array","items":{"type":"string"}},{"type":"string"}],"description":"SUM aggregate(s): {col:alias}, [col], or \"col,alias\""},"count":{"oneOf":[{"type":"object","additionalProperties":{"type":"string"}},{"type":"array","items":{"type":"string"}},{"type":"string"}],"description":"COUNT aggregate(s): {col:alias}, [col], or \"col,alias\""},"sort":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}],"description":"ORDER BY: array or \"col [asc|desc], col2\""},"page":{"type":"integer","minimum":1,"description":"1-based page; pair with size - size alone defaults to page 1."},"size":{"type":"integer","minimum":1,"maximum":500,"description":"rows per page (1..500); may be used alone"}},"additionalProperties":{"description":"plain column condition: value equals, or \">,100\"-style compare prefix"}}})"),
         toolQuery);
  b.tool(Json(R"({"name":"db_count","description":"Count rows in a table (optionally matching a where object whose keys are column conditions). Cheaper and clearer than db_query when only the number matters.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object","description":"column conditions, e.g. {\"status\":\"paid\"}"}}}})"),
         toolCount);
  b.tool(Json(R"({"name":"db_insert","description":"Insert ONE row (MRTR-confirmed). data is a column->value object; a missing id is auto-generated, an existing id upserts. For many rows use db_insert_batch.","inputSchema":{"type":"object","required":["db","table","data"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object","description":"column->value row"}}}})"),
         toolInsert);
  b.tool(Json(R"({"name":"db_insert_batch","description":"Insert/upsert MULTIPLE rows in one statement (MRTR-confirmed). rows is a non-empty array of column->value objects; on duplicate constraint values existing rows are updated (upsert). Prefer this over repeated db_insert for bulk loads.","inputSchema":{"type":"object","required":["db","table","rows"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"rows":{"type":"array","items":{"type":"object"},"minItems":1,"description":"row objects; keys of the first row define the column list"},"constraint":{"type":"string","description":"upsert conflict column, default id"}}}})"),
         toolInsertBatch);
  b.tool(Json(R"({"name":"db_update","description":"Update ONE row by id (MRTR-confirmed). data holds the columns to change; where MUST be {\"id\": <value>}. For condition-based updates use db_trans with a structured update element.","inputSchema":{"type":"object","required":["db","table","data","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object","description":"columns to change"},"where":{"type":"object","description":"must carry {\"id\": <value>}"}}}})"),
         toolUpdate);
  b.tool(Json(R"({"name":"db_delete","description":"Delete rows (MRTR-confirmed, destructive). where:{\"id\": v} deletes that row; where:{} deletes ALL rows in the table (the confirmation states the affected count). Other conditions are rejected - query ids with db_query then delete by id, or use db_trans for condition-based deletes.","inputSchema":{"type":"object","required":["db","table","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object","description":"{\"id\": v} for one row, {} for all"}}}})"),
         toolDelete);
}

}  // namespace zorm_mcp
