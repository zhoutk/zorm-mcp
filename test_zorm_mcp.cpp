// zorm-mcp - end-to-end test.
//
// Spawns the zorm_mcp_server executable as a child (real stdio pipes) and
// drives it with the mcp-cpp Client API - the exact path an AI client takes.
// Coverage goal: if this suite is green, a real agent client can basically
// use the server correctly. That means asserting not just tool outcomes but
// the WIRE SHAPES AI clients validate:
//   - every tools/call result carries the mandatory content array
//   - input_required results carry requestState + inputRequests.confirm
//   - errors are JSON-RPC InvalidParams with teaching messages
//
// The child runs with --child over the open-box defaults (sqlite3 + jsonfile
// in the exe's working directory); both scratch tables are cleaned at the
// start and end of each run so the suite is idempotent.

#include <mcp/mcp.hpp>
#if defined(MCP_HAS_HTTP)
#include <mcp/transport_http.hpp>
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {

struct Child {
  HANDLE inWrite = nullptr, outRead = nullptr;
  PROCESS_INFORMATION pi{};
  ~Child() {
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (pi.hProcess) {
      // Graceful wait, then hard-kill: a stuck child must never leak into the
      // terminal (a leaked child once froze the shell it was spawned from).
      if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
      }
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    DWORD written = 0;
    return WriteFile(inWrite, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    DWORD got = 0;
    while (ReadFile(outRead, &ch, 1, &got, nullptr) && got == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
  void closeStdin() {
    if (inWrite) {
      CloseHandle(inWrite);
      inWrite = nullptr;
    }
  }
  void terminate() {
    if (pi.hProcess) {
      TerminateProcess(pi.hProcess, 0);
      WaitForSingleObject(pi.hProcess, 2000);
      CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
      CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
  }
  int waitExit() {
    DWORD code = 0;
    if (pi.hProcess) {
      WaitForSingleObject(pi.hProcess, 10000);
      GetExitCodeProcess(pi.hProcess, &code);
    }
    return static_cast<int>(code);
  }
};

Child spawnChild(bool confirmWrites, bool allowSql,
                 const std::vector<std::string>& extraArgs = {}) {
#ifdef ZORM_DLL_DIR
  // The server child loads zorm's runtime DLLs; make sure they resolve even
  // when ctest runs outside the run-test PATH setup.
  char pathBuf[8192] = {0};
  GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf));
  std::string newPath = ZORM_DLL_DIR ";" + std::string(pathBuf);
  SetEnvironmentVariableA("PATH", newPath.c_str());
#endif
  // Spawn the REAL server (sibling of this test executable) - NOT this test
  // itself: the test's main is gtest_main with no --child handler, so
  // self-spawning would recursively run the whole suite (fork bomb).
  char testPath[MAX_PATH];
  GetModuleFileNameA(nullptr, testPath, MAX_PATH);
  std::string serverPath(testPath);
  const size_t slash = serverPath.find_last_of("\\/");
  serverPath = serverPath.substr(0, slash + 1) + "zorm_mcp_server.exe";

  std::string cmdline = std::string("\"") + serverPath + "\" --child";
  if (!confirmWrites) cmdline += " --yes";
  if (allowSql) cmdline += " --allow-sql";
  for (const auto& arg : extraArgs) cmdline += " " + arg;
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, outWrite = nullptr;
  Child c;
  EXPECT_TRUE(CreatePipe(&inRead, &c.inWrite, &sa, 0));
  EXPECT_TRUE(CreatePipe(&c.outRead, &outWrite, &sa, 0));
  if (c.inWrite) SetHandleInformation(c.inWrite, HANDLE_FLAG_INHERIT, 0);
  if (c.outRead) SetHandleInformation(c.outRead, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.dwFlags |= STARTF_USESTDHANDLES;
  EXPECT_TRUE(CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             nullptr, &si, &c.pi));
  CloseHandle(inRead);
  CloseHandle(outWrite);
  return c;
}

}  // namespace
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct Child {
  int inWrite = -1, outRead = -1;
  pid_t pid = -1;
  ~Child() {
    if (inWrite >= 0) close(inWrite);
    if (outRead >= 0) close(outRead);
    if (pid > 0) waitpid(pid, nullptr, 0);
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    return ::write(inWrite, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    while (::read(outRead, &ch, 1) == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
  void closeStdin() {
    if (inWrite >= 0) {
      close(inWrite);
      inWrite = -1;
    }
  }
  void terminate() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
  int waitExit() {
    int status = 0;
    if (pid > 0) {
      waitpid(pid, &status, 0);
      pid = -1;
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return -1;
    }
    return 0;
  }
};
Child spawnChild(bool confirmWrites, bool allowSql,
                 const std::vector<std::string>& extraArgs = {}) {
  // Spawn the real server executable sitting next to this test (never
  // self-spawn: this test's main is gtest_main without --child).
  std::string self = "/proc/self/exe";
  char buf[4096] = {0};
  ssize_t n = readlink(self.c_str(), buf, sizeof(buf) - 1);
  std::string serverPath = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : self;
  const size_t slash = serverPath.find_last_of('/');
  serverPath = serverPath.substr(0, slash + 1) + "zorm_mcp_server";

  int inPipe[2], outPipe[2];
  pipe(inPipe); pipe(outPipe);
  Child c;
  c.pid = fork();
  if (c.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    std::vector<char*> childArgv = {const_cast<char*>("zorm_mcp_server"),
                                    const_cast<char*>("--child")};
    std::string yesFlag, sqlFlag;
    if (!confirmWrites) {
      yesFlag = "--yes";
      childArgv.push_back(const_cast<char*>(yesFlag.c_str()));
    }
    if (allowSql) {
      sqlFlag = "--allow-sql";
      childArgv.push_back(const_cast<char*>(sqlFlag.c_str()));
    }
    for (const auto& arg : extraArgs)
      childArgv.push_back(const_cast<char*>(arg.c_str()));
    childArgv.push_back(nullptr);
    execv(serverPath.c_str(), childArgv.data());
    _exit(127);
  }
  close(inPipe[0]); close(outPipe[1]);
  c.inWrite = inPipe[1]; c.outRead = outPipe[0];
  return c;
}
}  // namespace
#endif

namespace {

// A pipe-backed ITransport so the mcp-cpp Client drives the child directly.
struct PipeTransport final : ITransport {
  Child& child;
  explicit PipeTransport(Child& c) : child(c) {}
  McpResult<std::optional<std::string>> read() override {
    std::string line;
    if (!child.readLine(line)) return Ok(std::optional<std::string>(std::nullopt));
    return Ok(std::optional<std::string>(std::move(line)));
  }
  McpResult<void> write(std::string_view message) override {
    if (!child.writeLine(std::string(message)))
      return Err(McpErrorCode::InternalError, "write to child failed");
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }
};

const char* kE2eTable = "t_e2e";

// Builds {"db": dbName, ...extra} call arguments.
Json argsWith(const char* dbName) {
  Json a;
  a.add("db", Json::str(dbName));
  return a;
}

// Spec compliance: every tools/call result - success, input_required, or
// cancel - must carry a content array (Claude Code rejects results without).
void expectEnvelope(const Json& result) {
  ASSERT_TRUE(result.isObject());
  ASSERT_TRUE(result["content"].isArray()) << result.toString();
  ASSERT_GE(result["content"].size(), 1u);
  EXPECT_EQ(result["content"][0]["type"].toString(), "text");
}

// db_exec_sql convenience (child must run --allow-sql).
McpResult<Json> execSql(Client& client, const std::string& sql) {
  Json a = argsWith("sqlite3");
  a.add("sql", Json::str(sql));
  return client.callTool("db_exec_sql", a);
}

// Calls a write tool and, when the (possibly MRTR-confirming) server answers
// input_required, completes the confirmation with approved:true. This is how
// fixtures stay correct under BOTH --yes and confirmation-on children.
McpResult<Json> callWrite(Client& client, const char* tool, Json args) {
  auto rs = client.callTool(tool, args);
  if (rs.has_value() && (*rs)["resultType"].toString() == "input_required") {
    // Per MCP MRTR the retry carries requestState/inputResponses at the
    // tools/call params level, not inside `arguments`.
    rs = client.retryToolCall(tool, args, *rs,
                              Json(R"({"confirm":{"approved":true}})"));
  }
  return rs;
}

// Creates the shared e2e table and empties it (idempotent across runs).
void resetTable(Client& client) {
  Json create = argsWith("sqlite3");
  create.add("sql", Json::str(std::string("create table if not exists ") + kE2eTable +
                              " (id varchar(32) primary key, name varchar(32), score double)"));
  auto rs = callWrite(client, "db_exec_sql", create);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
  Json del = argsWith("sqlite3");
  del.add("sql", Json::str(std::string("delete from ") + kE2eTable));
  rs = callWrite(client, "db_exec_sql", del);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
}

// Empties a jsonfile scratch table through the structured tools (the path a
// delete-all takes on the backend with no delete-all SQL support).
void clearJsonTable(Client& client, const char* table) {
  Json d = argsWith("jsonfile");
  d.add("table", Json::str(table));
  d.add("where", Json("{}"));
  auto rs = callWrite(client, "db_delete", d);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  ASSERT_EQ((*rs)["status"].toInt(), 200) << rs->toString();
}

long long countOf(Client& client, const char* db, const char* table) {
  Json c = argsWith(db);
  c.add("table", Json::str(table));
  auto rs = client.callTool("db_count", c);
  EXPECT_TRUE(rs.has_value());
  if (!rs.has_value()) return -1;
  const Json data = member(*rs, "data");
  return (data.isArray() && data.size() > 0) ? data[0]["total"].toInt64() : -1;
}

bool hasTool(const Json& toolsArr, const char* name) {
  for (const auto& t : toolsArr.toVector()) {
    if (t["name"].toString() == name) return true;
  }
  return false;
}

}  // namespace

// ---- protocol surface -----------------------------------------------------------

TEST(ZormMcpE2E, DiscoverToolsListAndGating) {
  // --allow-sql OFF: db_exec_sql must be hidden from tools/list entirely.
  Child child = spawnChild(false, false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value());
  EXPECT_EQ((*disc)["resultType"].toString(), "complete");
  // The AI-comprehension layer: instructions must ride the discovery result.
  EXPECT_NE(member(*disc, "instructions").toString().find("db_describe_table"),
            std::string::npos);
  EXPECT_NE(member(*disc, "instructions").toString().find("Backends: {"),
            std::string::npos);

  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value());
  const Json toolsArr = member(*tools, "tools");
  ASSERT_TRUE(toolsArr.isArray());
  EXPECT_TRUE(hasTool(toolsArr, "db_list_tables"));
  EXPECT_TRUE(hasTool(toolsArr, "db_describe_table"));
  EXPECT_TRUE(hasTool(toolsArr, "db_indexes"));
  EXPECT_TRUE(hasTool(toolsArr, "db_info"));
  EXPECT_TRUE(hasTool(toolsArr, "db_query"));
  EXPECT_TRUE(hasTool(toolsArr, "db_count"));
  EXPECT_TRUE(hasTool(toolsArr, "db_query_sql"));
  EXPECT_TRUE(hasTool(toolsArr, "db_insert"));
  EXPECT_TRUE(hasTool(toolsArr, "db_insert_batch"));
  EXPECT_TRUE(hasTool(toolsArr, "db_update"));
  EXPECT_TRUE(hasTool(toolsArr, "db_delete"));
  EXPECT_TRUE(hasTool(toolsArr, "db_trans"));
  EXPECT_FALSE(hasTool(toolsArr, "db_exec_sql"));
  // Descriptions must teach routing (the AI reads these on every request).
  for (const auto& tool : toolsArr.toVector()) {
    EXPECT_FALSE(tool["description"].toString().empty()) << tool["name"].toString();
  }

  // Pull channels: playbook prompt + guide resource + schema resource.
  auto pg = client.request("prompts/get", Json(R"({"name":"db-playbook"})"));
  ASSERT_TRUE(pg.has_value());
  EXPECT_NE((*pg).toString().find("MRTR"), std::string::npos);
  auto guide = client.readResource("db://guide");
  ASSERT_TRUE(guide.has_value());
  EXPECT_NE((*guide).toString().find("db_describe_table"), std::string::npos);
  auto overview = client.readResource("db://schema");
  ASSERT_TRUE(overview.has_value());
  EXPECT_NE((*overview).toString().find("sqlite3"), std::string::npos);
  EXPECT_NE((*overview).toString().find("jsonfile"), std::string::npos);
}

// Strict clients (VS Code / Copilot) reject a tool schema whose array type has
// no `items`. Walk every schema node and assert the invariant holds after the
// db_query_sql/db_exec_sql `values` regression.
static void checkArraySchemas(const Json& node, const std::string& path,
                              std::vector<std::string>& problems) {
  if (node.isArray()) {
    const auto items = node.toVector();
    for (size_t i = 0; i < items.size(); ++i)
      checkArraySchemas(items[i], path + "[" + std::to_string(i) + "]", problems);
    return;
  }
  if (!node.isObject()) return;
  if (member(node, "type").toString() == "array" && !node.contains("items")) {
    problems.push_back(path + ": array without items");
  }
  for (const auto& k : node.getAllKeys().toVector()) {
    const std::string key = k.toString();
    checkArraySchemas(node[key], path + "/" + key, problems);
  }
}

TEST(ZormMcpE2E, ToolSchemasSatisfyStrictArrayValidation) {
  Child child = spawnChild(false, true);  // --allow-sql: include db_exec_sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value()) << tools.error().message;
  const Json arr = member(*tools, "tools");
  ASSERT_TRUE(arr.isArray());
  std::vector<std::string> problems;
  for (const auto& tool : arr.toVector()) {
    const std::string name = member(tool, "name").toString();
    checkArraySchemas(member(tool, "inputSchema"), name, problems);
  }
  EXPECT_TRUE(problems.empty());
  for (const auto& p : problems) ADD_FAILURE() << p;

  // The specific shape Copilot flagged.
  EXPECT_NE(tools->toString().find("\"values\":{\"type\":\"array\",\"items\":{}"),
            std::string::npos);
}

// ---- metadata: the "get table fields" capability ---------------------------------

TEST(ZormMcpE2E, MetadataOnEmptyTable) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // db_describe_table on the EMPTY table - catalog metadata must report full
  // structure with zero rows present (this is what query-the-table cannot do).
  auto desc = client.callTool("db_describe_table",
                              Json(R"({"db":"sqlite3","table":"t_e2e"})"));
  ASSERT_TRUE(desc.has_value()) << desc.error().message;
  expectEnvelope(*desc);
  const Json cols = member(*desc, "columns");
  ASSERT_TRUE(cols.isArray());
  ASSERT_EQ(cols.size(), 3u);  // id / name / score
  EXPECT_EQ(cols[0]["name"].toString(), "id");
  EXPECT_TRUE(cols[0]["primary_key"].isTrue());
  EXPECT_NE(cols[1]["type"].toString(), "");          // name : varchar
  EXPECT_NE(cols[2]["type"].toString().find("double"), std::string::npos);
  EXPECT_TRUE(cols[1]["nullable"].isTrue());

  // Per-table resource reads the same metadata. The table was created AFTER the
  // server started, so this only works because the parametric URIs are URI
  // templates resolved live - not a startup snapshot.
  auto res = client.readResource("db://schema/sqlite3/t_e2e");
  ASSERT_TRUE(res.has_value()) << res.error().message;
  EXPECT_NE((*res).toString().find("primary key"), std::string::npos);

  // The templates are advertised for client discovery.
  auto tmpls = client.listResourceTemplates();
  ASSERT_TRUE(tmpls.has_value()) << tmpls.error().message;
  const std::string tmplText = (*tmpls).toString();
  EXPECT_NE(tmplText.find("db://schema/{db}"), std::string::npos);
  EXPECT_NE(tmplText.find("db://schema/{db}/{table}"), std::string::npos);

  // db_list_tables + db_indexes + db_info.
  auto tables = client.callTool("db_list_tables", Json(R"({"db":"sqlite3"})"));
  ASSERT_TRUE(tables.has_value());
  EXPECT_NE((*tables).toString().find("t_e2e"), std::string::npos);
  auto idx = client.callTool("db_indexes",
                             Json(R"({"db":"sqlite3","table":"t_e2e"})"));
  ASSERT_TRUE(idx.has_value()) << idx.error().message;  // pk autoindex at least
  auto info = client.callTool("db_info", Json(R"({"db":"sqlite3"})"));
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ((*info)["type"].toString(), "sqlite3");
  EXPECT_NE(member(*info, "server_version").toString(), "");

  // Negative: unknown db lists the available backends (self-correcting).
  auto badDb = client.callTool("db_list_tables", Json(R"({"db":"oracle"})"));
  ASSERT_FALSE(badDb.has_value());
  EXPECT_EQ(badDb.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(badDb.error().message.find("jsonfile"), std::string::npos);
  auto badTable = client.callTool("db_describe_table",
                                  Json(R"({"db":"sqlite3","table":"nope"})"));
  ASSERT_FALSE(badTable.has_value());
  EXPECT_EQ(badTable.error().code, McpErrorCode::InvalidParams);
}

// ---- the full data plane on sqlite ----------------------------------------------

TEST(ZormMcpE2E, DataPlaneEndToEnd) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));
  const char* tb = "t_e2e";

  // db_insert: one row with explicit id (status 200 + content envelope).
  Json ins = argsWith("sqlite3");
  ins.add("table", Json::str(tb));
  ins.add("data", Json(R"({"id":"r1","name":"ada","score":95.5})"));
  auto rs = client.callTool("db_insert", ins);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  expectEnvelope(*rs);
  EXPECT_EQ((*rs)["status"].toInt(), 200);

  // db_query: compare-prefix condition.
  auto qr = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","score":">,50"})"));
  ASSERT_TRUE(qr.has_value());
  EXPECT_NE((*qr).toString().find("ada"), std::string::npos);

  // db_query: fields projection + ins (IN list).
  auto pr = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","fields":["name"],"ins":{"id":["r1","r2"]}})"));
  ASSERT_TRUE(pr.has_value());
  const std::string projText = (*pr).toString();
  EXPECT_NE(projText.find("name"), std::string::npos);

  // db_query: lks fuzzy + sort + page/size pagination envelope.
  Json fq = argsWith("sqlite3");
  fq.add("table", Json::str(tb));
  fq.add("lks", Json(R"({"name":"%a%"})"));
  fq.add("sort", Json(R"(["score"])"));
  fq.add("page", Json(1));
  fq.add("size", Json(2));
  auto fr = client.callTool("db_query", fq);
  ASSERT_TRUE(fr.has_value()) << fr.error().message;

  // db_query: group + sum aggregation.
  Json gq = argsWith("sqlite3");
  gq.add("table", Json::str(tb));
  gq.add("group", Json(R"(["name"])"));
  gq.add("sum", Json(R"({"score":"total"})"));
  auto gr = client.callTool("db_query", gq);
  ASSERT_TRUE(gr.has_value()) << gr.error().message;
  EXPECT_NE((*gr).toString().find("total"), std::string::npos);

  // db_count with where.
  Json cw = argsWith("sqlite3");
  cw.add("table", Json::str(tb));
  cw.add("where", Json(R"({"name":"ada"})"));
  auto cnt = client.callTool("db_count", cw);
  ASSERT_TRUE(cnt.has_value());
  const Json cntData = member(*cnt, "data");
  ASSERT_TRUE(cntData.isArray() && cntData.size() > 0);
  EXPECT_EQ(cntData[0]["total"].toInt64(), 1);

  // db_update (single row by id).
  Json up = argsWith("sqlite3");
  up.add("table", Json::str(tb));
  up.add("data", Json(R"({"score":88})"));
  up.add("where", Json(R"({"id":"r1"})"));
  rs = client.callTool("db_update", up);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);

  // db_update rejects a where without id (with guidance, not silence).
  Json badUp = argsWith("sqlite3");
  badUp.add("table", Json::str(tb));
  badUp.add("data", Json(R"({"score":1})"));
  badUp.add("where", Json(R"({"name":"ada"})"));
  rs = client.callTool("db_update", badUp);
  ASSERT_FALSE(rs.has_value());
  EXPECT_EQ(rs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(rs.error().message.find("db_trans"), std::string::npos);

  // db_query_sql: parameterized read with values binding.
  Json sel = argsWith("sqlite3");
  sel.add("sql", Json::str("select name from t_e2e where id = ?"));
  sel.add("values", Json(R"(["r1"])"));
  rs = client.callTool("db_query_sql", sel);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_NE((*rs).toString().find("ada"), std::string::npos);

  // db_query_sql: read-only gate rejects writes and statement smuggling.
  rs = client.callTool("db_query_sql",
                       Json(R"({"db":"sqlite3","sql":"delete from t_e2e"})"));
  ASSERT_FALSE(rs.has_value());
  EXPECT_EQ(rs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(rs.error().message.find("READ-ONLY"), std::string::npos);
  rs = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"select 1; delete from t_e2e"})"));
  ASSERT_FALSE(rs.has_value());

  // db_insert_batch: multi-row upsert (r1 overwritten, r2 inserted).
  Json batch = argsWith("sqlite3");
  batch.add("table", Json::str(tb));
  batch.add("rows",
            Json(R"([{"id":"r1","name":"ada2","score":10},{"id":"r2","name":"bob","score":20}])"));
  rs = client.callTool("db_insert_batch", batch);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 2);

  // db_trans: structured elements insert r3 + update r2, atomically.
  Json tr = argsWith("sqlite3");
  tr.add("sqls",
         Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r3","name":"carol","score":30}},{"table":"t_e2e","method":"update","params":{"score":99},"id":"r2"}])"));
  rs = client.callTool("db_trans", tr);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 3);

  // db_trans: raw text element with values binding.
  Json tr2 = argsWith("sqlite3");
  tr2.add("sqls",
          Json(R"json([{"text":"insert into t_e2e (id, name, score) values (?, ?, ?)","values":["r4","dave",40]}])json"));
  rs = client.callTool("db_trans", tr2);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 4);

  // db_delete: single row by id.
  Json del = argsWith("sqlite3");
  del.add("table", Json::str(tb));
  del.add("where", Json(R"({"id":"r4"})"));
  rs = client.callTool("db_delete", del);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 3);

  // db_delete with a condition-only where is REJECTED with guidance.
  Json badDel = argsWith("sqlite3");
  badDel.add("table", Json::str(tb));
  badDel.add("where", Json(R"({"name":"carol"})"));
  rs = client.callTool("db_delete", badDel);
  ASSERT_FALSE(rs.has_value());
  EXPECT_NE(rs.error().message.find("db_query"), std::string::npos);

  // db_delete where:{} = delete all.
  Json delAll = argsWith("sqlite3");
  delAll.add("table", Json::str(tb));
  delAll.add("where", Json("{}"));
  rs = client.callTool("db_delete", delAll);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", tb), 0);

  // completion: table names starting with "t_e2".
  Json extra;
  extra.add("ref", Json(R"({"type":"ref/prompt","name":"table","arguments":{"db":"sqlite3"}})"));
  Json arg;
  arg.add("name", Json::str("table"));
  arg.add("value", Json::str("t_e2"));
  extra.add("argument", std::move(arg));
  auto comp = client.request("completion/complete", extra);
  ASSERT_TRUE(comp.has_value());
  EXPECT_NE((*comp).toString().find("t_e2e"), std::string::npos);
}

// ---- validation and failure paths -----------------------------------------------

TEST(ZormMcpE2E, ValidationAndWriteFailurePaths) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // Data-plane tools validate `db` like the metadata tools do.
  auto noDb = client.callTool("db_query",
                              Json(R"({"db":"oracle","table":"t_e2e"})"));
  ASSERT_FALSE(noDb.has_value());
  EXPECT_EQ(noDb.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(noDb.error().message.find("sqlite3"), std::string::npos);
  // The tool inputSchema is enforced before the handler: required fields and
  // minItems reject malformed calls as InvalidParams.
  auto noTable = client.callTool("db_query", Json(R"({"db":"sqlite3"})"));
  ASSERT_FALSE(noTable.has_value());
  EXPECT_EQ(noTable.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(noTable.error().message.find("table"), std::string::npos);
  auto emptyBatch = client.callTool(
      "db_insert_batch", Json(R"({"db":"sqlite3","table":"t_e2e","rows":[]})"));
  ASSERT_FALSE(emptyBatch.has_value());
  EXPECT_EQ(emptyBatch.error().code, McpErrorCode::InvalidParams);
  auto emptyTrans = client.callTool(
      "db_trans", Json(R"({"db":"sqlite3","sqls":[]})"));
  ASSERT_FALSE(emptyTrans.has_value());
  EXPECT_EQ(emptyTrans.error().code, McpErrorCode::InvalidParams);
  auto badTable = client.callTool(
      "db_delete",
      Json(R"({"db":"sqlite3","table":"bad name","where":{}})"));
  ASSERT_FALSE(badTable.has_value());
  EXPECT_NE(badTable.error().message.find("invalid table name"), std::string::npos);

  // Metadata identifier validation + the jsonfile "no indexes" branch.
  auto badDescribe = client.callTool(
      "db_describe_table", Json(R"({"db":"sqlite3","table":"bad-name"})"));
  ASSERT_FALSE(badDescribe.has_value());
  EXPECT_NE(badDescribe.error().message.find("invalid table name"), std::string::npos);
  auto badIndexes = client.callTool(
      "db_indexes", Json(R"({"db":"sqlite3","table":"bad-name"})"));
  ASSERT_FALSE(badIndexes.has_value());
  EXPECT_NE(badIndexes.error().message.find("invalid table name"), std::string::npos);
  auto jsonIndexes = client.callTool(
      "db_indexes", Json(R"({"db":"jsonfile","table":"t_e2e_json"})"));
  ASSERT_TRUE(jsonIndexes.has_value()) << jsonIndexes.error().message;
  EXPECT_NE(jsonIndexes->toString().find("no index concept"), std::string::npos);
  auto jsonInfo = client.callTool("db_info", Json(R"({"db":"jsonfile"})"));
  ASSERT_TRUE(jsonInfo.has_value()) << jsonInfo.error().message;
  EXPECT_EQ((*jsonInfo)["type"].toString(), "jsonfile");
  EXPECT_TRUE((*jsonInfo)["ready"].isTrue());

  // A driver-level write failure names the file and carries the raw message.
  auto failed = client.callTool(
      "db_insert",
      Json(R"({"db":"sqlite3","table":"no_such_table","data":{"id":"x"}})"));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code, McpErrorCode::InternalError);
  EXPECT_NE(failed.error().message.find("write failed"), std::string::npos);
  EXPECT_NE(failed.error().message.find("[file:"), std::string::npos);
}

// ---- regressions from docs/TEST_REPORT.md ----------------------------------------

TEST(ZormMcpE2E, ReportedDefectRegressions) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  for (const char* id : {"r1", "r2", "r3"}) {
    Json ins = argsWith("sqlite3");
    ins.add("table", Json::str("t_e2e"));
    ins.add("data", Json(std::string(R"({"id":")") + id +
                         R"(","name":"n","score":10})"));
    ASSERT_TRUE(client.callTool("db_insert", ins).has_value());
  }

  // D-03: ins:{col:[v1,v2]} means ONE column with N values.
  auto insArr = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","ins":{"id":["r1","r2"]}})"));
  ASSERT_TRUE(insArr.has_value()) << insArr.error().message;
  EXPECT_EQ(member(*insArr, "data").size(), 2u) << insArr->toString();

  // D-05: string aggregate specs are "col,alias" (not doubled).
  auto cntAlias = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","count":"*,total"})"));
  ASSERT_TRUE(cntAlias.has_value()) << cntAlias.error().message;
  EXPECT_EQ((*cntAlias)["data"][0]["total"].toInt64(), 3);
  auto sumAlias = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","sum":"score,total"})"));
  ASSERT_TRUE(sumAlias.has_value()) << sumAlias.error().message;
  EXPECT_TRUE((*sumAlias)["data"][0]["total"].isNumber()) << sumAlias->toString();

  // D-10: size alone paginates; page alone is rejected.
  auto sizeOnly = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","size":2})"));
  ASSERT_TRUE(sizeOnly.has_value()) << sizeOnly.error().message;
  EXPECT_EQ(member(*sizeOnly, "data").size(), 2u);
  EXPECT_EQ((*sizeOnly)["records"].toInt64(), 3);
  auto pageOnly = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","page":2})"));
  ASSERT_FALSE(pageOnly.has_value());
  EXPECT_NE(pageOnly.error().message.find("page requires size"), std::string::npos);

  // D-11: leading comments are allowed; the keyword after them still gates.
  auto commented = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"/* c */ select 1 as x"})"));
  EXPECT_TRUE(commented.has_value()) << commented.error().message;
  auto hiddenWrite = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"-- c\n delete from t_e2e"})"));
  ASSERT_FALSE(hiddenWrite.has_value());

  // D-02: every structured identifier is validated before any SQL is built.
  auto badTable = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e where 1=1 --"})"));
  ASSERT_FALSE(badTable.has_value());
  EXPECT_NE(badTable.error().message.find("invalid table name"), std::string::npos);
  auto badField = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","fields":["id from t_e2e --"]})"));
  ASSERT_FALSE(badField.has_value());
  EXPECT_NE(badField.error().message.find("invalid field name"), std::string::npos);
  auto badSort = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","sort":["score desc; drop table t_e2e"]})"));
  ASSERT_FALSE(badSort.has_value());
  EXPECT_NE(badSort.error().message.find("invalid sort"), std::string::npos);
  auto badCondition = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","id = 1 or 1":"x"})"));
  ASSERT_FALSE(badCondition.has_value());
  EXPECT_NE(badCondition.error().message.find("invalid condition column name"),
            std::string::npos);
  auto badColumn = client.callTool(
      "db_insert",
      Json(R"({"db":"sqlite3","table":"t_e2e","data":{"id;drop":"x"}})"));
  ASSERT_FALSE(badColumn.has_value());
  EXPECT_NE(badColumn.error().message.find("invalid column name"), std::string::npos);
  auto badTrans = client.callTool(
      "db_trans",
      Json(R"({"db":"sqlite3","sqls":[{"table":"t_e2e where 1=1 --","method":"delete","params":{}}]})"));
  ASSERT_FALSE(badTrans.has_value());
  EXPECT_NE(badTrans.error().message.find("invalid transaction table name"),
            std::string::npos);

  // Nothing was executed by the rejected payloads.
  auto cnt = client.callTool(
      "db_count", Json(R"({"db":"sqlite3","table":"t_e2e"})"));
  ASSERT_TRUE(cnt.has_value());
  EXPECT_EQ((*cnt)["data"][0]["total"].toInt64(), 3);

  // db_exec_sql now binds `values` (previously ignored) and reports both
  // affected/affectedRows (D-12).
  Json exec = argsWith("sqlite3");
  exec.add("sql", Json::str(
      "insert into t_e2e (id, name, score) values (?, ?, ?)"));
  exec.add("values", Json(R"(["r4","dave",40])"));
  auto execRs = client.callTool("db_exec_sql", exec);
  ASSERT_TRUE(execRs.has_value()) << execRs.error().message;
  EXPECT_TRUE((*execRs)["affected"].isNumber());
  EXPECT_TRUE((*execRs)["affectedRows"].isNumber());
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 4);

  // D-01: the default jsonfile data file is zorm-mcp-data.json, never the
  // zorm-mcp.json config template.
  const char* tb = "e2e_default_json";
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));
  Json ji = argsWith("jsonfile");
  ji.add("table", Json::str(tb));
  ji.add("data", Json(R"({"note":"default-file"})"));
  ASSERT_TRUE(client.callTool("db_insert", ji).has_value());
  // The child inherits this process's cwd, so the default file lands here.
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::current_path() / "zorm-mcp-data.json"));
}

// Regressions from the 2026-10-22 field report (F1/F3/F5/F6/O1). F2/F4/F7 live
// in zorm (dm8 fetch loop, jsonfile SQL subset, dm8 diagnostic output).
TEST(ZormMcpE2E, FieldReport20261022Regressions) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  for (const char* id : {"f1a", "f1b", "f1c"}) {
    Json ins = argsWith("sqlite3");
    ins.add("table", Json::str("t_e2e"));
    ins.add("data", Json(std::string(R"({"id":")") + id +
                         R"(","name":"n","score":10})"));
    ASSERT_TRUE(client.callTool("db_insert", ins).has_value());
  }

  auto querySql = [&](const char* sql) {
    return client.callTool("db_query_sql",
                           Json(std::string(R"({"db":"sqlite3","sql":")") + sql +
                                R"("})"));
  };

  // F1: the first keyword proves nothing. Data-modifying CTEs and EXPLAIN
  // ANALYZE (which really runs the statement on PostgreSQL) must be rejected
  // by the gate, not merely by the engine.
  for (const char* sql : {
           "with c as (select 1 as x) delete from t_e2e",
           "with c as (select 1 as x) update t_e2e set score = 0",
           "with c as (select 1 as x) insert into t_e2e (id) values ('f1z')",
           "explain analyze update t_e2e set score = 0",
           "explain analyze delete from t_e2e"}) {
    auto rs = querySql(sql);
    ASSERT_FALSE(rs.has_value()) << sql;
    EXPECT_NE(rs.error().message.find("READ-ONLY"), std::string::npos) << sql;
  }
  // The same words inside literals/comments are not statements.
  for (const char* sql : {
           "with c as (select 1 as x) select * from c",
           "select 'delete from t_e2e' as s",
           "/* delete from t_e2e */ select 1 as s",
           "select replace('abc','b','x') as s"}) {
    auto rs = querySql(sql);
    EXPECT_TRUE(rs.has_value()) << sql << ": " << rs.error().message;
  }
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 3);

  // F3: more than one aggregate produced illegal SQL ("... as total count(*)
  // as n" - missing comma). Multi-metric analysis is a primary use case.
  auto multi = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","group":["name"],"sum":"score,total","count":"id,n"})"));
  ASSERT_TRUE(multi.has_value()) << multi.error().message;
  ASSERT_EQ(member(*multi, "data").size(), 1u) << multi->toString();
  EXPECT_EQ((*multi)["data"][0]["total"].toInt64(), 30);
  EXPECT_EQ((*multi)["data"][0]["n"].toInt64(), 3);
  auto twoSums = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","sum":"score,total,score,double"})"));
  ASSERT_TRUE(twoSums.has_value()) << twoSums.error().message;
  EXPECT_TRUE((*twoSums)["data"][0]["double"].isNumber()) << twoSums->toString();
  auto twoCounts = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","count":"id,n,score,m"})"));
  ASSERT_TRUE(twoCounts.has_value()) << twoCounts.error().message;
  EXPECT_EQ((*twoCounts)["data"][0]["m"].toInt64(), 3);

  // F5: the spec's resource-template ref carries only `uri`; completion must
  // work without the non-standard ref.name.
  auto byUri = client.complete(
      Json(R"j({"type":"ref/resource","uri":"db://schema/{db}"})j"),
      Json(R"j({"name":"db","value":"sql"})j"));
  ASSERT_TRUE(byUri.has_value()) << byUri.error().message;
  EXPECT_NE(byUri->toString().find("sqlite3"), std::string::npos)
      << byUri->toString();
  auto tableByUri = client.complete(
      Json(R"j({"type":"ref/resource","uri":"db://schema/{db}/{table}"})j"),
      Json(R"j({"name":"table","value":"t_e"})j"));
  ASSERT_TRUE(tableByUri.has_value()) << tableByUri.error().message;
  EXPECT_NE(tableByUri->toString().find("t_e2e"), std::string::npos)
      << tableByUri->toString();

  // O1: a comma inside an ors/lks/ins value cannot ride the pair encoding -
  // say why and point at db_query_sql, do not answer "ors is wrong".
  auto comma = client.callTool(
      "db_query", Json(R"({"db":"sqlite3","table":"t_e2e","ors":{"score":">,35"}})"));
  ASSERT_FALSE(comma.has_value());
  EXPECT_NE(comma.error().message.find("contains a comma"), std::string::npos)
      << comma.error().message;
  EXPECT_NE(comma.error().message.find("db_query_sql"), std::string::npos);

  // F4 (tool layer): jsonfile's SQL subset is explicit, never a silent [].
  auto jfWhere = client.callTool(
      "db_query_sql",
      Json(R"({"db":"jsonfile","sql":"select * from t_e2e_json where name = 'ada'"})"));
  ASSERT_FALSE(jfWhere.has_value());
  EXPECT_NE(jfWhere.error().message.find("supports only"), std::string::npos)
      << jfWhere.error().message;
  auto jfMissing = client.callTool(
      "db_query_sql", Json(R"({"db":"jsonfile","sql":"select * from no_such_jf"})"));
  ASSERT_FALSE(jfMissing.has_value());
  EXPECT_NE(jfMissing.error().message.find("unknown table"), std::string::npos)
      << jfMissing.error().message;
}

// Residual vectors from REGRESSION_REPORT_2026-10-22 §5. The gate used to look
// for a SELECT anywhere after EXPLAIN (so "EXPLAIN ANALYZE CREATE TABLE x AS
// SELECT" got the SELECT it wanted and PostgreSQL really created the table) and
// accepted every PRAGMA (so "PRAGMA user_version = 42" persisted to the file).
// Both are now whitelists: EXPLAIN must wrap SELECT/WITH/VALUES, PRAGMA must be
// a known read-only name.
TEST(ZormMcpE2E, ReadOnlyGateResidualVectors) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  auto querySql = [&](const char* sql) {
    return client.callTool(
        "db_query_sql",
        Json(std::string(R"({"db":"sqlite3","sql":")") + sql + R"("})"));
  };

  // R1/R3: EXPLAIN of DDL/DCL/EXECUTE and of DML.
  for (const char* sql : {
           "explain create table t_e2e_gate as select 1",
           "explain analyze create table t_e2e_gate as select 1",
           "explain analyze create materialized view t_e2e_gate_mv as select 1",
           "explain analyze execute some_stmt",
           "explain analyze declare c cursor for select 1",
           "explain analyze call some_proc()",
           "explain delete from t_e2e",
           "explain query plan delete from t_e2e"}) {
    auto rs = querySql(sql);
    ASSERT_FALSE(rs.has_value()) << sql;
    EXPECT_NE(rs.error().message.find("READ-ONLY"), std::string::npos) << sql;
  }
  // R2: PRAGMA writes (both '=' and the function-call assignment form).
  for (const char* sql : {
           "pragma user_version = 424242",
           "pragma user_version(424242)",
           "pragma journal_mode = delete",
           "pragma journal_mode(wal)",
           "pragma writable_schema = on",
           "pragma incremental_vacuum",
           "pragma optimize"}) {
    auto rs = querySql(sql);
    ASSERT_FALSE(rs.has_value()) << sql;
    EXPECT_NE(rs.error().message.find("READ-ONLY"), std::string::npos) << sql;
  }
  // What the gate must keep admitting.
  for (const char* sql : {
           "select 1 as x",
           "explain select 1",
           "explain query plan select 1",
           "with c as (select 1 as x) select * from c",
           "pragma table_info(t_e2e)",
           "pragma index_list(t_e2e)",
           "pragma user_version",
           "pragma database_list"}) {
    auto rs = querySql(sql);
    EXPECT_TRUE(rs.has_value()) << sql << ": " << rs.error().message;
  }
  // Nothing was created by the rejected DDL above.
  auto created = querySql(
      "select name from sqlite_master where name like 't_e2e_gate%'");
  ASSERT_TRUE(created.has_value()) << created.error().message;
  EXPECT_EQ(created->toString().find("t_e2e_gate"), std::string::npos)
      << created->toString();
}

// Findings from the 2026-10-23 field report (F-1/F-2/F-4/F-9). Each was a
// silent or unhelpful behaviour an AI client could misread as success.
TEST(ZormMcpE2E, FieldReport20261023Findings) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));
  for (const auto& row : {R"({"id":"a","name":"ada","score":10})",
                          R"({"id":"b","name":"bob","score":20})",
                          R"({"id":"c","name":"cy","score":30})"}) {
    Json ins = argsWith("sqlite3");
    ins.add("table", Json::str("t_e2e"));
    ins.add("data", Json(row));
    ASSERT_TRUE(client.callTool("db_insert", ins).has_value());
  }

  // F-1: an unknown top-level key used to be dropped silently, so
  // {"score":10} returned the WHOLE table. It must name the key and the shape.
  auto dropped = client.callTool(
      "db_count", Json(R"({"db":"sqlite3","table":"t_e2e","score":10})"));
  ASSERT_FALSE(dropped.has_value()) << dropped->toString();
  EXPECT_NE(dropped.error().message.find("unknown argument 'score'"),
            std::string::npos)
      << dropped.error().message;
  EXPECT_NE(dropped.error().message.find("where"), std::string::npos);
  auto scoped = client.callTool(
      "db_count", Json(R"({"db":"sqlite3","table":"t_e2e","where":{"score":10}})"));
  ASSERT_TRUE(scoped.has_value()) << scoped.error().message;
  EXPECT_EQ((*scoped)["data"][0]["total"].toInt64(), 1);

  // F-2: a functional PRAGMA through db_query_sql used to render every row as
  // "{}". The columns now come from the prepared statement.
  auto pragma = client.callTool(
      "db_query_sql", Json(R"sql({"db":"sqlite3","sql":"pragma table_info(t_e2e)"})sql"));
  ASSERT_TRUE(pragma.has_value()) << pragma.error().message;
  EXPECT_NE(pragma->toString().find("\"name\":\"id\""), std::string::npos)
      << pragma->toString();

  // F-4: the anonymous "*" in all three shapes means count(*) - it used to
  // generate "count(*) as *" (syntax error). Explicit aliases are unchanged.
  for (const char* args : {
           R"({"db":"sqlite3","table":"t_e2e","count":["*"]})",
           R"({"db":"sqlite3","table":"t_e2e","count":"*"})",
           R"({"db":"sqlite3","table":"t_e2e","count":{"*":""}})"}) {
    auto rs = client.callTool("db_query", Json(args));
    ASSERT_TRUE(rs.has_value()) << args << ": " << rs.error().message;
    EXPECT_EQ((*rs)["data"][0]["total"].toInt64(), 3) << args;
  }

  // F-9: the sqlite text error used to be "Error Code : 20" only; the driver
  // detail makes an INTEGER PRIMARY KEY + generated hex id self-correctable.
  auto create = client.callTool("db_exec_sql",
                                Json(R"({"db":"sqlite3","sql":"drop table if exists f9_intpk_e2e"})"));
  ASSERT_TRUE(create.has_value()) << create.error().message;
  create = client.callTool(
      "db_exec_sql",
      Json(R"sql({"db":"sqlite3","sql":"create table f9_intpk_e2e (id INTEGER PRIMARY KEY, v text)"})sql"));
  ASSERT_TRUE(create.has_value()) << create.error().message;
  auto mismatch = client.callTool(
      "db_insert", Json(R"({"db":"sqlite3","table":"f9_intpk_e2e","data":{"v":"x"}})"));
  ASSERT_FALSE(mismatch.has_value()) << mismatch->toString();
  EXPECT_NE(mismatch.error().message.find("datatype mismatch"), std::string::npos)
      << mismatch.error().message;
  client.callTool("db_exec_sql",
                  Json(R"({"db":"sqlite3","sql":"drop table f9_intpk_e2e"})"));
}

// F6: a db_delete WHERE the tool cannot express must be rejected while STAGING,
// so the user is never asked to approve an operation that can only fail.
TEST(ZormMcpE2E, DeleteConditionRejectedBeforeConfirmation) {
  Child child = spawnChild(true, false);  // MRTR confirmation ON
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto rs = client.callTool(
      "db_delete", Json(R"({"db":"sqlite3","table":"t_e2e","where":{"score":10}})"));
  ASSERT_FALSE(rs.has_value()) << rs->toString();
  EXPECT_EQ(rs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(rs.error().message.find("supports only where"), std::string::npos)
      << rs.error().message;
  // Not an input_required result: nothing was staged for the user to approve.
  EXPECT_EQ(rs.error().message.find("Confirm"), std::string::npos);

  // F-10: a one-element transaction reads "1 statement", not "1 statements".
  Json tr = argsWith("sqlite3");
  tr.add("sqls",
         Json(R"([{"table":"t_e2e","method":"delete","params":{}}])"));
  auto staged = client.callTool("db_trans", tr);
  ASSERT_TRUE(staged.has_value()) << staged->toString();
  EXPECT_NE(staged->toString().find("(1 statement)"), std::string::npos)
      << staged->toString();
  EXPECT_EQ(staged->toString().find("1 statements"), std::string::npos);
}

// ---- the open-box jsonfile backend (same semantics, no SQL) ----------------------

TEST(ZormMcpE2E, JsonfileParity) {
  Child child = spawnChild(false, false);  // jsonfile path needs no --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  const char* tb = "t_e2e_json";
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));

  // db_insert auto-generates the id when absent.
  Json ins = argsWith("jsonfile");
  ins.add("table", Json::str(tb));
  ins.add("data", Json(R"({"name":"ada","score":11})"));
  auto rs = client.callTool("db_insert", ins);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  expectEnvelope(*rs);
  EXPECT_EQ((*rs)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "jsonfile", tb), 1);

  // db_describe_table reads the storage format's columns array - no rows
  // needed, no SQL involved.
  auto desc = client.callTool("db_describe_table",
                              Json(R"({"db":"jsonfile","table":"t_e2e_json"})"));
  ASSERT_TRUE(desc.has_value()) << desc.error().message;
  EXPECT_NE((*desc).toString().find("name"), std::string::npos);
  EXPECT_NE((*desc).toString().find("score"), std::string::npos);

  // Smart query on jsonfile (202 semantics = empty result, still 200 here).
  Json q = argsWith("jsonfile");
  q.add("table", Json::str(tb));
  q.add("lks", Json(R"({"name":"%ad%"})"));
  rs = client.callTool("db_query", q);
  ASSERT_TRUE(rs.has_value());
  EXPECT_NE((*rs).toString().find("ada"), std::string::npos);

  // delete-all works even though jsonfile's SQL shim cannot express it.
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));
  EXPECT_EQ(countOf(client, "jsonfile", tb), 0);
}

// ---- MRTR write confirmation ------------------------------------------------------

TEST(ZormMcpE2E, MrtrWriteConfirmation) {
  Child child = spawnChild(true, false);  // confirmation ON, no --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  const char* tb = "e2e_mrtr";
  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));

  // First call: input_required + requestState + elicitation inputRequests.
  Json args = argsWith("jsonfile");
  args.add("table", Json::str(tb));
  args.add("data", Json(R"({"note":"e2e-zorm-mcp"})"));
  auto first = client.callTool("db_insert", args);
  ASSERT_TRUE(first.has_value());
  expectEnvelope(*first);
  EXPECT_EQ((*first)["resultType"].toString(), "input_required");
  const std::string state = (*first)["requestState"].toString();
  EXPECT_FALSE(state.empty());
  const Json confirmReq = member(*first, "inputRequests");
  ASSERT_TRUE(confirmReq.isObject());
  EXPECT_EQ(confirmReq["confirm"]["method"].toString(), "elicitation/create");
  // Confirmation text names the target (db.table) and how to approve.
  const std::string msg = (*first)["content"][0]["text"].toString();
  EXPECT_NE(msg.find("jsonfile." + std::string(tb)), std::string::npos);
  EXPECT_NE(msg.find(state), std::string::npos);

  // Approved retry: spec-level retry (requestState + inputResponses at the
  // params root) -> execute.
  Json retry = argsWith("jsonfile");
  retry.add("table", Json::str(tb));
  retry.add("data", Json(R"({"note":"e2e-zorm-mcp"})"));
  auto second = client.retryToolCall("db_insert", retry, *first,
                                     Json(R"({"confirm":{"approved":true}})"));
  ASSERT_TRUE(second.has_value());
  expectEnvelope(*second);
  EXPECT_EQ((*second)["status"].toInt(), 200);
  ASSERT_EQ(countOf(client, "jsonfile", tb), 1);

  // Rejected retry: staging again then answering false must NOT write.
  Json stage2 = argsWith("jsonfile");
  stage2.add("table", Json::str(tb));
  stage2.add("data", Json(R"({"note":"e2e-rejected"})"));
  auto req2 = client.callTool("db_insert", stage2);
  ASSERT_TRUE(req2.has_value());
  EXPECT_EQ((*req2)["resultType"].toString(), "input_required");
  Json decline = argsWith("jsonfile");
  decline.add("table", Json::str(tb));
  decline.add("data", Json(R"({"note":"e2e-rejected"})"));
  auto declined = client.retryToolCall("db_insert", decline, *req2,
                                       Json(R"({"confirm":{"approved":false}})"));
  ASSERT_TRUE(declined.has_value());
  EXPECT_EQ((*declined)["resultType"].toString(), "complete");
  EXPECT_NE((*declined).toString().find("cancelled"), std::string::npos);
  ASSERT_EQ(countOf(client, "jsonfile", tb), 1);

  // Unknown/forged requestState -> InvalidParams with re-stage guidance.
  Json stale = argsWith("jsonfile");
  stale.add("table", Json::str(tb));
  stale.add("data", Json(R"({"note":"x"})"));
  Json forged = Json(R"({"requestState":"st-999999"})");
  auto errRs = client.retryToolCall("db_insert", stale, forged,
                                    Json(R"({"confirm":{"approved":true}})"));
  ASSERT_FALSE(errRs.has_value());
  EXPECT_EQ(errRs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(errRs.error().message.find("stage a new one"), std::string::npos);

  // A tampered (single-character) state is rejected by the codec.
  Json stageT = argsWith("jsonfile");
  stageT.add("table", Json::str(tb));
  stageT.add("data", Json(R"({"note":"tamper"})"));
  auto reqT = client.callTool("db_insert", stageT);
  ASSERT_TRUE(reqT.has_value());
  std::string tampered = (*reqT)["requestState"].toString();
  ASSERT_GT(tampered.size(), 12u);
  tampered[tampered.size() / 2] =
      tampered[tampered.size() / 2] == 'A' ? 'B' : 'A';
  Json tamperedPrev = Json(R"({})");
  tamperedPrev.add("requestState", Json::str(tampered));
  auto tamperedRs = client.retryToolCall("db_insert", stageT, tamperedPrev,
                                         Json(R"({"confirm":{"approved":true}})"));
  ASSERT_FALSE(tamperedRs.has_value());
  EXPECT_EQ(tamperedRs.error().code, McpErrorCode::InvalidParams);
  EXPECT_NE(tamperedRs.error().message.find("invalid, expired, forged"),
            std::string::npos);
  EXPECT_EQ(countOf(client, "jsonfile", tb), 1);

  // A state issued for one tool cannot be replayed against another.
  Json stageX = argsWith("jsonfile");
  stageX.add("table", Json::str(tb));
  stageX.add("data", Json(R"({"note":"cross-tool"})"));
  auto reqX = client.callTool("db_insert", stageX);
  ASSERT_TRUE(reqX.has_value());
  Json crossArgs = argsWith("jsonfile");
  crossArgs.add("table", Json::str(tb));
  crossArgs.add("data", Json(R"({"note":"cross-tool"})"));
  crossArgs.add("where", Json(R"({"id":"nope"})"));
  auto crossRs = client.retryToolCall("db_update", crossArgs, *reqX,
                                      Json(R"({"confirm":{"approved":true}})"));
  ASSERT_FALSE(crossRs.has_value());
  EXPECT_EQ(crossRs.error().code, McpErrorCode::InvalidParams);
  EXPECT_EQ(countOf(client, "jsonfile", tb), 1);

  // Flat-form approval ({"approved":true}) also executes.
  Json stage3 = argsWith("jsonfile");
  stage3.add("table", Json::str(tb));
  stage3.add("data", Json(R"({"note":"e2e-flat"})"));
  auto req3 = client.callTool("db_insert", stage3);
  ASSERT_TRUE(req3.has_value());
  Json flat = argsWith("jsonfile");
  flat.add("table", Json::str(tb));
  flat.add("data", Json(R"({"note":"e2e-flat"})"));
  auto flatRs = client.retryToolCall("db_insert", flat, *req3,
                                     Json(R"({"approved":true})"));
  ASSERT_TRUE(flatRs.has_value());
  EXPECT_EQ((*flatRs)["status"].toInt(), 200);

  // An unrecognized inputResponses shape counts as NOT approved (fail-safe).
  Json stage4 = argsWith("jsonfile");
  stage4.add("table", Json::str(tb));
  stage4.add("data", Json(R"({"note":"e2e-garbage"})"));
  auto req4 = client.callTool("db_insert", stage4);
  ASSERT_TRUE(req4.has_value());
  auto garbage = client.retryToolCall("db_insert", stage4, *req4,
                                      Json(R"({"nonsense":"yes"})"));
  ASSERT_TRUE(garbage.has_value());
  EXPECT_NE((*garbage).toString().find("cancelled"), std::string::npos);

  // Placement guard: the old in-arguments form is rejected with guidance.
  Json misplaced = argsWith("jsonfile");
  misplaced.add("table", Json::str(tb));
  misplaced.add("data", Json(R"({"note":"misplaced"})"));
  misplaced.add("requestState", Json::str((*req4)["requestState"].toString()));
  // callTool puts requestState INSIDE arguments here - exactly what the
  // server must reject rather than silently re-stage.
  auto misplacedRs = client.callTool("db_insert", misplaced);
  ASSERT_FALSE(misplacedRs.has_value());
  EXPECT_NE(misplacedRs.error().message.find("params level"), std::string::npos);

  // One-shot semantics: an already-consumed state cannot execute again.
  Json stage5 = argsWith("jsonfile");
  stage5.add("table", Json::str(tb));
  stage5.add("data", Json(R"({"note":"once"})"));
  auto req5 = client.callTool("db_insert", stage5);
  ASSERT_TRUE(req5.has_value());
  auto once = client.retryToolCall("db_insert", stage5, *req5,
                                   Json(R"({"confirm":{"approved":true}})"));
  ASSERT_TRUE(once.has_value());
  auto replayed = client.retryToolCall("db_insert", stage5, *req5,
                                       Json(R"({"confirm":{"approved":true}})"));
  ASSERT_FALSE(replayed.has_value());
  EXPECT_NE(replayed.error().message.find("no pending"), std::string::npos);

  // Id-targeted delete stages with an impact estimate (count-first wording).
  Json del = argsWith("jsonfile");
  del.add("table", Json::str(tb));
  del.add("where", Json(R"({"id":"no-such-id"})"));
  auto delReq = client.callTool("db_delete", del);
  ASSERT_TRUE(delReq.has_value());
  ASSERT_EQ((*delReq)["resultType"].toString(), "input_required");
  EXPECT_NE((*delReq)["content"][0]["text"].toString().find("0 rows"),
            std::string::npos);
  auto delDone = client.retryToolCall("db_delete", del, *delReq,
                                      Json(R"({"confirm":{"approved":true}})"));
  ASSERT_TRUE(delDone.has_value()) << delDone.error().message;

  ASSERT_NO_FATAL_FAILURE(clearJsonTable(client, tb));
  EXPECT_EQ(countOf(client, "jsonfile", tb), 0);
}

// ---- protocol eras, capabilities, negotiation hints -------------------------------

TEST(ZormMcpE2E, ModernDiscoverAdvertisesV04Surface) {
  Child child = spawnChild(false, false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);

  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value()) << disc.error().message;
  const Json versions = member(*disc, "supportedVersions");
  ASSERT_TRUE(versions.isArray());
  const std::string versionText = versions.toString();
  EXPECT_NE(versionText.find("2026-07-28"), std::string::npos);
  EXPECT_NE(versionText.find("2025-11-25"), std::string::npos);

  const Json caps = member(*disc, "capabilities");
  ASSERT_TRUE(caps.isObject());
  EXPECT_TRUE(member(caps, "tools").isObject());
  EXPECT_TRUE(member(caps, "prompts").isObject());
  EXPECT_TRUE(member(caps, "resources").isObject());
  // mcp-cpp 0.4 declarations: writes publish resource updates, and resources
  // are list-changeable at runtime (tables can be created/dropped).
  const Json resources = member(caps, "resources");
  EXPECT_TRUE(member(resources, "subscribe").isTrue()) << caps.toString();
  EXPECT_TRUE(member(resources, "listChanged").isTrue()) << caps.toString();

  // CacheableResult hints are mandatory on modern */list results.
  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value()) << tools.error().message;
  EXPECT_TRUE((*tools)["ttlMs"].isNumber()) << tools->toString();
  EXPECT_EQ((*tools)["cacheScope"].toString(), "public");
  // Every modern result carries serverInfo in _meta (D8).
  EXPECT_EQ((*tools)["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].toString(),
            "zorm-mcp");

  auto guide = client.readResource("db://guide");
  ASSERT_TRUE(guide.has_value()) << guide.error().message;
  EXPECT_TRUE((*guide)["ttlMs"].isNumber());
  EXPECT_EQ((*guide)["cacheScope"].toString(), "public");

  // Era auto-negotiation (E2) picks the newest mutually supported revision.
  auto era = client.negotiateEra();
  ASSERT_TRUE(era.has_value()) << era.error().message;
  EXPECT_TRUE(era->modern);
  EXPECT_EQ(era->protocolVersion, "2026-07-28");
}

TEST(ZormMcpE2E, LegacyClientsWithoutMetaAreServed) {
  // Raw wire: the 2026-07-28 gates require params._meta, but this server opts
  // into dual-era mode so pre-2026 clients (Claude Code / VS Code on older
  // revisions) can call without it.
  Child child = spawnChild(false, false);
  PipeTransport t(child);

  ASSERT_TRUE(t.write(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
                  .has_value());
  std::string line;
  ASSERT_TRUE(child.readLine(line)) << "no response to legacy tools/list";
  std::string err;
  const Json list = Json::ParseJson(line, err);
  ASSERT_FALSE(list.isError()) << line;
  EXPECT_TRUE(member(list, "result").isObject()) << line;
  EXPECT_EQ(member(list, "result")["tools"].size() > 0, true);

  ASSERT_TRUE(
      t.write(R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
              R"("params":{"name":"db_list_tables","arguments":{"db":"sqlite3"}}})")
          .has_value());
  ASSERT_TRUE(child.readLine(line));
  const Json call = Json::ParseJson(line, err);
  ASSERT_FALSE(call.isError()) << line;
  // db_list_tables answers {tables, count} directly (no zorm status field).
  EXPECT_TRUE(member(call, "result")["tables"].isArray()) << line;
}

TEST(ZormMcpE2E, ResourceSubscriptionReceivesWriteNotifications) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  std::vector<std::string> methods;
  std::vector<std::string> payloads;
  client.onNotification([&](const JsonRpcNotification& n) {
    methods.push_back(n.method);
    payloads.push_back(n.params.toString());
  });
  auto ack = client.subscribe(Json(
      R"({"notifications":{"resourcesListChanged":true,"resourceSubscriptions":["db://schema/sqlite3/t_e2e"]}})"));

  ASSERT_TRUE(ack.has_value()) << ack.error().message;

  // The write publishes resources/updated + resources/list_changed. They are
  // handed to onNotification while the NEXT exchange waits for its response.
  Json ins = argsWith("sqlite3");
  ins.add("table", Json::str("t_e2e"));
  ins.add("data", Json(R"({"id":"r-sub","name":"sub","score":1})"));
  auto rs = client.callTool("db_insert", ins);
  ASSERT_TRUE(rs.has_value()) << rs.error().message;
  (void)client.callTool("db_count", Json(R"({"db":"sqlite3","table":"t_e2e"})"));

  bool sawUpdated = false;
  bool sawListChanged = false;
  for (size_t i = 0; i < methods.size(); ++i) {
    if (methods[i] == "notifications/resources/updated") {
      sawUpdated = payloads[i].find("db://schema/sqlite3/t_e2e") != std::string::npos;
    }
    if (methods[i] == "notifications/resources/list_changed") sawListChanged = true;
  }
  EXPECT_TRUE(sawUpdated) << methods.size() << " notifications observed";
  EXPECT_TRUE(sawListChanged);
  client.closeSubscriptions();
}

// ---- SQL gate, resources, and configuration edge cases -----------------------------

TEST(ZormMcpE2E, SqlGateAndResourceEdgeCases) {
  Child child = spawnChild(false, true);  // --yes --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // Every allowlisted read-only first keyword is accepted.
  for (const char* sql : {"pragma table_info(t_e2e)",
                          "with x as (select 1 as n) select * from x",
                          "explain select * from t_e2e"}) {
    auto rs = client.callTool("db_query_sql",
                              Json(std::string(R"({"db":"sqlite3","sql":")") + sql +
                                   R"("})"));
    EXPECT_TRUE(rs.has_value()) << sql << ": " << rs.error().message;
  }
  // A keyword that is only a PREFIX of an identifier is not allowed.
  auto prefix = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"selectivity from t_e2e"})"));
  ASSERT_FALSE(prefix.has_value());
  EXPECT_NE(prefix.error().message.find("READ-ONLY"), std::string::npos);

  // Empty SQL and a bad table name both teach the next move.
  auto empty = client.callTool("db_query_sql", Json(R"({"db":"sqlite3","sql":""})"));
  ASSERT_FALSE(empty.has_value());
  EXPECT_NE(empty.error().message.find("sql is required"), std::string::npos);
  auto bad = client.callTool(
      "db_query_sql", Json(R"({"db":"sqlite3","sql":"select * from no_such_table"})"));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, McpErrorCode::InvalidParams);

  // db_query variants not exercised by the main data-plane test.
  auto ors = client.callTool(
      "db_query",
      Json(R"({"db":"sqlite3","table":"t_e2e","ors":{"id":"r1","name":"nobody"}})"));
  EXPECT_TRUE(ors.has_value()) << ors.error().message;
  Json fuzzy = argsWith("sqlite3");
  fuzzy.add("table", Json::str("t_e2e"));
  fuzzy.add("fuzzy", Json(1));
  fuzzy.add("sort", Json::str("score desc"));
  fuzzy.add("count", Json::str("id"));
  auto fz = client.callTool("db_query", fuzzy);
  EXPECT_TRUE(fz.has_value()) << fz.error().message;

  // Resource error branches.
  auto unknownPrefix = client.readResource("db://nope");
  ASSERT_FALSE(unknownPrefix.has_value());
  EXPECT_NE(unknownPrefix.error().message.find("resource"), std::string::npos);
  auto unknownDb = client.readResource("db://schema/oracle");
  ASSERT_FALSE(unknownDb.has_value());
  EXPECT_NE(unknownDb.error().message.find("available"), std::string::npos);
  auto unknownTable = client.readResource("db://schema/sqlite3/no_such");
  ASSERT_FALSE(unknownTable.has_value());
  EXPECT_NE(unknownTable.error().message.find("unknown table"), std::string::npos);
  auto dbOnly = client.readResource("db://schema/sqlite3");
  ASSERT_TRUE(dbOnly.has_value()) << dbOnly.error().message;
  EXPECT_NE((*dbOnly).toString().find("t_e2e"), std::string::npos);

  // Completion for db (prefix) and fields (column names).
  // The handler routes on ref.name (the completion argument name).
  auto dbComp = client.complete(
      Json(R"({"type":"ref/prompt","name":"db","arguments":{}})"),
      Json(R"({"name":"db","value":"sql"})"));
  ASSERT_TRUE(dbComp.has_value()) << dbComp.error().message;
  EXPECT_NE(dbComp->toString().find("sqlite3"), std::string::npos);
  auto colComp = client.complete(
      Json(R"({"type":"ref/prompt","name":"fields","arguments":{"db":"sqlite3","table":"t_e2e"}})"),
      Json(R"({"name":"fields","value":"sc"})"));
  ASSERT_TRUE(colComp.has_value()) << colComp.error().message;
  EXPECT_NE(colComp->toString().find("score"), std::string::npos);
}

TEST(ZormMcpE2E, ConfigFileAndCli) {
  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() /
      ("zorm-mcp-cfg-" + std::to_string(static_cast<long long>(
                             std::chrono::steady_clock::now().time_since_epoch().count())));
  fs::create_directories(dir);
  const fs::path cfg = dir / "cfg.json";
  const fs::path dbFile = dir / "custom.db";
  {
    std::ofstream out(cfg, std::ios::binary);
    out << R"({"databases":{"cfgdb":{"type":"sqlite3","options":{)"
        << R"("connString":")" << dbFile.generic_string()
        << R"(","DbLogClose":true,"parameterized":true}}}})";
  }

  {
    Child child = spawnChild(false, false, {"--config", cfg.generic_string()});
    PipeTransport t(child);
    Client client("e2e", "1.0");
    client.connect(t);
    auto tables = client.callTool("db_list_tables", Json(R"({"db":"cfgdb"})"));
    ASSERT_TRUE(tables.has_value()) << tables.error().message;
    EXPECT_TRUE(member(*tables, "tables").isArray());
    auto info = client.callTool("db_info", Json(R"({"db":"cfgdb"})"));
    ASSERT_TRUE(info.has_value()) << info.error().message;
    EXPECT_EQ((*info)["type"].toString(), "sqlite3");
    // A config file REPLACES the built-in defaults.
    auto missing = client.callTool("db_list_tables", Json(R"({"db":"sqlite3"})"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find("cfgdb"), std::string::npos);
  }

  // D-01: refuse to open a zorm-mcp CONFIG file as the jsonfile database.
  const fs::path configAsData = dir / "config-as-data.json";
  {
    std::ofstream out(configAsData, std::ios::binary);
    out << R"({"databases":{"x":{"type":"sqlite3","options":{}}}})";
  }
  const fs::path clashCfg = dir / "clash.json";
  {
    std::ofstream out(clashCfg, std::ios::binary);
    out << R"({"databases":{"j":{"type":"jsonfile","options":{)"
        << R"("connString":")" << configAsData.generic_string()
        << R"(","DbLogClose":true}}}})";
  }
  {
    Child child = spawnChild(false, false, {"--config", clashCfg.generic_string()});
    EXPECT_EQ(child.waitExit(), 1);
  }

  // Malformed config -> exit code 2 (usage/config error), no server loop.
  const fs::path badCfg = dir / "bad.json";
  {
    std::ofstream out(badCfg, std::ios::binary);
    out << "{not json";
  }
  {
    Child child = spawnChild(false, false, {"--config", badCfg.generic_string()});
    EXPECT_EQ(child.waitExit(), 2);
  }

  // --help prints the flag reference and exits 0.
  {
    Child child = spawnChild(false, false, {"--help"});
    std::string line;
    bool sawUsage = false;
    bool sawTokenFlag = false;
    while (child.readLine(line)) {
      if (line.find("usage:") != std::string::npos) sawUsage = true;
      if (line.find("--http-token") != std::string::npos) sawTokenFlag = true;
    }
    EXPECT_TRUE(sawUsage);
    EXPECT_TRUE(sawTokenFlag);
    EXPECT_EQ(child.waitExit(), 0);
  }

  // Unknown flag -> usage error (exit 2).
  {
    Child child = spawnChild(false, false, {"--bogus-flag"});
    EXPECT_EQ(child.waitExit(), 2);
  }

#if defined(MCP_HAS_HTTP)
  // A chattering SSE keep-alive is a validated misconfiguration, not accepted.
  {
    Child child = spawnChild(false, false,
                             {"--http", "0", "--sse-keepalive", "5"});
    EXPECT_EQ(child.waitExit(), 2);
  }
#endif

  fs::remove_all(dir);
}

// ---- optional remote dialect matrix -------------------------------------------------
//
// Opt-in (CI stays offline): set ZORM_MCP_REMOTE_CONFIG to a zorm-mcp config
// file containing mysql / postgres / dm8 entries, e.g.
//   set ZORM_MCP_REMOTE_CONFIG=D:\codes\zorm-mcp\.tmp-remote.json
// The test creates a scratch table per backend, verifies the dialect fixes,
// and drops it afterwards.
TEST(ZormMcpE2E, RemoteBackendsMatrix) {
  const char* cfgPath = std::getenv("ZORM_MCP_REMOTE_CONFIG");
  if (cfgPath == nullptr || *cfgPath == '\0') {
    GTEST_SKIP() << "set ZORM_MCP_REMOTE_CONFIG to run the remote matrix";
  }
  std::ifstream in(cfgPath, std::ios::binary);
  ASSERT_TRUE(in) << "cannot read " << cfgPath;
  std::string raw((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  std::string err;
  const Json cfg = Json::ParseJson(raw, err);
  ASSERT_FALSE(cfg.isError()) << err;
  const Json dbs = member(cfg, "databases");
  ASSERT_TRUE(dbs.isObject()) << cfgPath;

  Child child = spawnChild(false, true, {"--config", cfgPath});
  PipeTransport t(child);
  Client client("e2e-remote", "1.0");
  client.connect(t);

  for (const char* backend : {"mysql", "postgres", "dm8"}) {
    const std::string db = backend;
    const Json spec = member(dbs, db);
    if (!spec.isObject()) continue;  // not configured
    auto info = client.callTool("db_info", Json(std::string(R"({"db":")") + db + R"("})"));
    if (!info.has_value()) {
      EXPECT_NE(info.error().message.find("unknown db"), std::string::npos)
          << backend << ": " << info.error().message;
      continue;
    }

    const bool isMysql = db == "mysql";
    const bool isPg = db == "postgres";
    const char* q = isMysql ? "`" : "\"";
    std::string schema;
    const Json opts = member(spec, "options");
    if (db == "dm8" && ZJSON::hasChild(opts, "db_name"))
      schema = opts["db_name"].toString();
    const std::string tableName = "mcp_fix_test";
    const std::string qualified =
        db == "dm8" && !schema.empty() && schema != "null"
            ? std::string("\"") + schema + "\"." + q + tableName + q
            : std::string(q) + tableName + q;
    const std::string kind = isPg ? "double precision" : "double";

    auto exec = [&](const std::string& sql) {
      Json a = argsWith(db.c_str());
      a.add("sql", Json::str(sql));
      return client.callTool("db_exec_sql", a);
    };
    (void)exec("drop table if exists " + qualified);
    auto created = exec("create table " + qualified + " (" + q + "id" + q +
                        " varchar(32) primary key, " + q + "name" + q +
                        " varchar(32), " + q + "age" + q + " integer, " + q +
                        "score" + q + " " + kind + ")");
    ASSERT_TRUE(created.has_value()) << backend << ": " << created.error().message;

    Json rows = Json(R"([])");
    for (const auto& [id, name, age] :
         {std::tuple<const char*, const char*, int>{"a", "ada", 18},
          {"b", "bob", 20}, {"c", "carol", 30}}) {
      Json row;
      row.add("id", Json::str(id));
      row.add("name", Json::str(name));
      row.add("age", Json(age));
      row.add("score", Json(10));
      rows.push_back(row);
    }
    Json batch = argsWith(db.c_str());
    batch.add("table", Json::str(tableName));
    batch.add("rows", rows);
    auto seeded = client.callTool("db_insert_batch", batch);
    ASSERT_TRUE(seeded.has_value()) << backend << ": " << seeded.error().message;

    // D-03: one column, multiple values.
    Json qins = argsWith(db.c_str());
    qins.add("table", Json::str(tableName));
    qins.add("ins", Json(R"({"age":[18,20]})"));
    auto ins = client.callTool("db_query", qins);
    ASSERT_TRUE(ins.has_value()) << backend << ": " << ins.error().message;
    EXPECT_EQ(member(*ins, "data").size(), 2u) << backend;

    // D-05: alias forms.
    Json qcount = argsWith(db.c_str());
    qcount.add("table", Json::str(tableName));
    qcount.add("count", Json::str("*,total"));
    auto cnt = client.callTool("db_query", qcount);
    ASSERT_TRUE(cnt.has_value()) << backend << ": " << cnt.error().message;
    EXPECT_TRUE((*cnt)["data"][0]["total"].isNumber()) << backend;
    EXPECT_EQ((*cnt)["data"][0]["total"].toInt64(), 3) << backend;

    // D-02: structured identifier injections are rejected, rows stay intact.
    Json inject = argsWith(db.c_str());
    inject.add("table", Json::str(tableName + " where 1=1 --"));
    auto inj = client.callTool("db_query", inject);
    ASSERT_FALSE(inj.has_value()) << backend;
    EXPECT_NE(inj.error().message.find("invalid table name"), std::string::npos);

    // D-10: size alone paginates.
    Json qpage = argsWith(db.c_str());
    qpage.add("table", Json::str(tableName));
    qpage.add("size", Json(2));
    auto page = client.callTool("db_query", qpage);
    ASSERT_TRUE(page.has_value()) << backend << ": " << page.error().message;
    EXPECT_EQ(member(*page, "data").size(), 2u) << backend;
    EXPECT_EQ((*page)["records"].toInt64(), 3) << backend;

    // D-07 (pg) / D-08 (mysql) / D-06 (dm8).
    if (isPg) {
      Json sql = argsWith(db.c_str());
      sql.add("sql", Json::str(
          std::string("select name from ") + qualified + " where age = ?"));
      sql.add("values", Json(R"([20])"));
      auto r = client.callTool("db_query_sql", sql);
      ASSERT_TRUE(r.has_value()) << r.error().message;
      EXPECT_EQ((*r)["data"][0]["name"].toString(), "bob");
    }
    if (isMysql) {
      Json sql = argsWith(db.c_str());
      sql.add("sql", Json::str("explain select * from " + qualified));
      auto r = client.callTool("db_query_sql", sql);
      ASSERT_TRUE(r.has_value()) << r.error().message;
      EXPECT_GT((*r)["data"].size(), 0u);
    }
    if (db == "dm8") {
      Json del = argsWith(db.c_str());
      del.add("table", Json::str(tableName));
      del.add("where", Json("{}"));
      auto r = client.callTool("db_delete", del);
      ASSERT_TRUE(r.has_value()) << r.error().message;
      auto after = client.callTool(
          "db_count", Json(std::string(R"({"db":"dm8","table":")") +
                           tableName + R"("})"));
      ASSERT_TRUE(after.has_value()) << after.error().message;
      EXPECT_EQ((*after)["data"][0]["total"].toInt64(), 0);
    }

    auto dropped = exec("drop table if exists " + qualified);
    EXPECT_TRUE(dropped.has_value()) << backend;
  }
}

// ---- HTTP transport: authentication, principal-bound MRTR --------------------------

#if defined(MCP_HAS_HTTP)
TEST(ZormMcpE2E, HttpBearerAuthAndPrincipalBoundState) {
  // Pick a high port. (A collision is possible in principle; the polling
  // below turns it into a clear failure rather than a hang.)
  std::mt19937 rng(std::random_device{}());
  const int port = 20000 + static_cast<int>(rng() % 40000);
  Child child = spawnChild(true, false,
                           {"--http", std::to_string(port), "--http-token", "tokA",
                            "--http-token", "tokB"});
  bool up = false;
  {
    mcp::http::HttpClientTransport probe("127.0.0.1", port);
    Client probeClient("e2e-probe", "1.0");
    probeClient.connect(probe);
    probe.setHeader("Authorization", "Bearer tokA");
    probeClient.setRequestTimeout(std::chrono::milliseconds(800));
    for (int i = 0; i < 50 && !up; ++i) {
      up = probeClient.discover().has_value();
      if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!up) child.terminate();
  ASSERT_TRUE(up) << "server never accepted HTTP on port " << port;

  mcp::http::HttpClientTransport t("127.0.0.1", port);
  Client client("e2e-http", "1.0");
  client.connect(t);
  client.setRequestTimeout(std::chrono::milliseconds(3000));

  // No Authorization header -> 401 (transport error, no JSON-RPC result).
  auto anonymous = client.discover();
  EXPECT_FALSE(anonymous.has_value());

  t.setHeader("Authorization", "Bearer tokA");
  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value()) << disc.error().message;

  // Stage a write as principal tokA.
  const char* tb = "e2e_http";
  Json clear = argsWith("jsonfile");
  clear.add("table", Json::str(tb));
  clear.add("where", Json("{}"));
  auto cleared = callWrite(client, "db_delete", clear);
  ASSERT_TRUE(cleared.has_value()) << cleared.error().message;

  Json ins = argsWith("jsonfile");
  ins.add("table", Json::str(tb));
  ins.add("data", Json(R"({"note":"principal-bound"})"));
  auto staged = client.callTool("db_insert", ins);
  ASSERT_TRUE(staged.has_value()) << staged.error().message;
  ASSERT_EQ((*staged)["resultType"].toString(), "input_required");

  // A DIFFERENT bearer cannot redeem tokA's sealed state (F1 / D7).
  t.setHeader("Authorization", "Bearer tokB");
  auto cross = client.retryToolCall("db_insert", ins, *staged,
                                    Json(R"({"confirm":{"approved":true}})"));
  ASSERT_FALSE(cross.has_value());
  EXPECT_EQ(cross.error().code, McpErrorCode::InvalidParams);

  // The original principal still can.
  t.setHeader("Authorization", "Bearer tokA");
  auto ok = client.retryToolCall("db_insert", ins, *staged,
                                 Json(R"({"confirm":{"approved":true}})"));
  ASSERT_TRUE(ok.has_value()) << ok.error().message;
  EXPECT_EQ((*ok)["status"].toInt(), 200);

  client.closeSubscriptions();
  child.terminate();
}
#endif  // MCP_HAS_HTTP

// ---- MRTR on the trans tool + graceful shutdown ------------------------------------

TEST(ZormMcpE2E, MrtrTransAndShutdown) {
  Child child = spawnChild(true, true);  // confirmation ON, --allow-sql
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  ASSERT_NO_FATAL_FAILURE(resetTable(client));

  // db_trans confirmation flow (impact text states the statement count).
  Json tr = argsWith("sqlite3");
  tr.add("sqls",
         Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r1","name":"ada","score":1}},{"table":"t_e2e","method":"insert","params":{"id":"r2","name":"bob","score":2}}])"));
  auto first = client.callTool("db_trans", tr);
  ASSERT_TRUE(first.has_value());
  expectEnvelope(*first);
  ASSERT_EQ((*first)["resultType"].toString(), "input_required");
  EXPECT_NE((*first)["content"][0]["text"].toString().find("2 statements"),
            std::string::npos);
  const std::string state = (*first)["requestState"].toString();
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 0);  // nothing executed yet

  Json retry = argsWith("sqlite3");
  retry.add("sqls",
            Json(R"([{"table":"t_e2e","method":"insert","params":{"id":"r1","name":"ada","score":1}},{"table":"t_e2e","method":"insert","params":{"id":"r2","name":"bob","score":2}}])"));
  auto second = client.retryToolCall("db_trans", retry, *first,
                                     Json(R"({"action":"accept"})"));
  ASSERT_TRUE(second.has_value()) << second.error().message;
  EXPECT_EQ((*second)["status"].toInt(), 200);
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 2);

  // --db_exec_sql hidden without --allow-sql is covered elsewhere; here it is
  // registered and confirmed: reject once, approve once.
  Json sql = argsWith("sqlite3");
  sql.add("sql", Json::str("insert into t_e2e (id, name, score) values ('r3','carol',3)"));
  auto req = client.callTool("db_exec_sql", sql);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ((*req)["resultType"].toString(), "input_required");
  Json sqlRetry = argsWith("sqlite3");
  sqlRetry.add("sql", Json::str("insert into t_e2e (id, name, score) values ('r3','carol',3)"));
  auto exec = client.retryToolCall("db_exec_sql", sqlRetry, *req,
                                   Json(R"({"confirm":{"approved":true}})"));
  ASSERT_TRUE(exec.has_value()) << exec.error().message;
  EXPECT_EQ(countOf(client, "sqlite3", "t_e2e"), 3);
}

TEST(ZormMcpE2E, GracefulShutdownOnEof) {
  Child child = spawnChild(false, false);
  PipeTransport t(child);
  Client client("e2e", "1.0");
  client.connect(t);
  auto disc = client.discover();
  ASSERT_TRUE(disc.has_value());
  // Closing the pipe's write end signals EOF; the server should exit cleanly.
  child.closeStdin();
  int status = 0;
#ifdef _WIN32
  EXPECT_TRUE(WaitForSingleObject(child.pi.hProcess, 10000) == WAIT_OBJECT_0);
#else
  EXPECT_EQ(waitpid(child.pid, &status, 0), child.pid);
#endif
}
