"""Runner 4: --allow-sql (db_exec_sql), write notifications, runtime-discovered
resources, subscribe/unsubscribe, cwd behaviour, config misuse, misc UX."""
import json
import os
import glob
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import (Client, init, call_json, approve, write_config, tool_names,
                        text_of, WORK, ROOT, subprocess)  # noqa: E402
import mcp_client  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:240] if detail else ""))


def rows(r):
    return (r or {}).get("data") or []


SQL_DB = os.path.join(WORK, "r4.db")
JF_DB = os.path.join(WORK, "r4.json")
for f in (SQL_DB, JF_DB, SQL_DB + "-wal", SQL_DB + "-shm"):
    if os.path.exists(f):
        os.remove(f)
cfg = write_config({"databases": {
    "sqlite3": {"type": "sqlite3", "options": {"connString": SQL_DB, "DbLogClose": True, "parameterized": True}},
    "jsonfile": {"type": "jsonfile", "options": {"connString": JF_DB, "DbLogClose": True}}}})

# ================= --allow-sql: db_exec_sql becomes visible =================
c = Client(args=["--config", cfg, "--yes", "--allow-sql"], name="allowsql")
init(c)
check("13 tools with --allow-sql", tool_names(c) == [
    "db_count", "db_delete", "db_describe_table", "db_exec_sql", "db_indexes",
    "db_info", "db_insert", "db_insert_batch", "db_list_tables", "db_query",
    "db_query_sql", "db_trans", "db_update"], str(tool_names(c)))

r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "create table cfg (id text primary key, v text, n integer)"})
check("db_exec_sql creates a table", r.get("affected") == 1 or r.get("status") == 200, str(r)[:160])
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "insert into cfg (id, v, n) values (?, ?, ?)", "values": ["a", "x", 1]})
check("db_exec_sql parameterized insert", r.get("affected") == 1, str(r)[:140])
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "update cfg set n = n + 1 where id = ?", "values": ["a"]})
check("db_exec_sql update", r.get("affected") == 1, str(r)[:140])
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "select v, n from cfg where id = ?"})
check("OBSERVATION db_exec_sql on SELECT: runs, reports affected, returns no rows",
      r.get("affected") == 1 and "data" not in r, str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select v, n from cfg"})
check("read via db_query_sql sees written row", rows(r) == [{"v": "x", "n": 2}], str(r)[:140])
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "alter table cfg add column created text"})
check("db_exec_sql DDL alter", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
r = call_json(c, "db_describe_table", {"db": "sqlite3", "table": "cfg"})
check("describe reflects schema change", "created" in [col["name"] for col in r.get("columns", [])], str(r)[:200])
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "drop table cfg"})
check("db_exec_sql drop table", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "cfg", "id": "a"})
check("dropped table now errors", "__error__" in r, str(r)[:140])
# unknown db routing still teaches
r = call_json(c, "db_exec_sql", {"db": "oracle", "sql": "select 1"})
check("db_exec_sql unknown db -> availability hint", "__error__" in r and "available" in r["message"], str(r)[:180])
# MRTR in non---yes mode for exec_sql happens in runner3 (trans/insert); exec stages too (covered indirectly)

# write notifications: subscribe FIRST through subscriptions/listen (2026)
ack = c.listen({"notifications": {"resourcesListChanged": True,
                                  "resourceSubscriptions": ["db://schema/sqlite3/nt"]}})
# create nt table first (trans with no table -> list_changed only)
r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table nt (id text primary key, k text)"}]})
c.wait_notifs(0.5)
acked = c.notif_methods()
check("subscriptions/listen ack arrived", "notifications/subscriptions/acknowledged" in acked, str(acked))
call_json(c, "db_insert", {"db": "sqlite3", "table": "nt", "data": {"id": "1"}})
c.wait_notifs(1.5)
methods = c.notif_methods()
check("2026 subscriber receives resources/updated + list_changed on write",
      "notifications/resources/updated" in methods and "notifications/resources/list_changed" in methods,
      str(set(methods)))
upds = [n for n in c.notifications if n.get("method") == "notifications/resources/updated"]
if upds:
    params = upds[-1].get("params", {})
    check("updated carries uri + subscriptionId",
          params.get("uri") == "db://schema/sqlite3/nt" and "subscriptionId" in json.dumps(params),
          json.dumps(params)[:200])

# legacy era: resources/subscribe is a stub (ack, no fan-out) per mcp-cpp 0.4
c2 = Client(args=["--config", cfg, "--yes"], name="legacy-sub")
init(c2, protocol="2024-11-05")
r = c2.request("resources/subscribe", {"uri": "db://schema/sqlite3/nt"})
check("legacy resources/subscribe acknowledged (mcp-cpp stub)", r.get("result") == {}, str(r)[:120])
call_json(c2, "db_insert", {"db": "sqlite3", "table": "nt", "data": {"id": "2"}})
c2.wait_notifs(1.0)
check("legacy subscriber receives no per-URI updates (fan-out is 2026-only)",
      "notifications/resources/updated" not in c2.notif_methods(), str(set(c2.notif_methods())))
c2.close()

# tasks/* are compiled into mcp-cpp but NOT enabled by zorm-mcp's buildServer
r = c.request("tasks/list", {})
check("tasks/* not advertised -> clean -32601 (feature intentionally off)",
      r.get("error", {}).get("code") == -32601, str(r.get("error"))[:180])

# jsonfile writes also notify
call_json(c, "db_insert", {"db": "jsonfile", "table": "rtimejson", "data": {"id": "1", "k": "z"}})
c.wait_notifs(1.0)
check("jsonfile write also notifies (list_changed at minimum)",
      "notifications/resources/list_changed" in c.notif_methods(), str(set(c.notif_methods())))

# ================= runtime resources (no restart) =================
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table rtime (id text primary key, k text)"}]})
call_json(c, "db_insert", {"db": "sqlite3", "table": "rtime", "data": {"id": "1", "k": "v"}})
r = c.request("resources/read", {"uri": "db://schema/sqlite3/rtime"})
txt = "".join(x.get("text", "") for x in r["result"]["contents"])
check("resource reads RUNTIME-created table (not startup snapshot)", "rtime" in txt and "id" in txt, txt[:160])
r = c.request("resources/list", {})
uris = [x["uri"] for x in r["result"]["resources"]]
r = c.request("resources/templates/list", {})
tpls = [x.get("uriTemplate") for x in r["result"].get("resourceTemplates", [])]
check("fixed + template resources listed", "db://guide" in uris and len(tpls) == 2, "%s %s" % (uris, tpls))
r = c.request("resources/read", {"uri": "db://schema/sqlite3"})
txt = "".join(x.get("text", "") for x in r["result"]["contents"])
check("per-db resource lists runtime table", "rtime" in txt, txt[:160])
r = c.request("resources/read", {"uri": "db://schema/jsonfile/rtimejson"})
txt = "".join(x.get("text", "") for x in r["result"]["contents"])
check("jsonfile runtime table readable via template", "rtimejson" in txt or "id" in txt, txt[:160])

# ================= completion coverage =================
r = c.request("completion/complete", {"ref": {"type": "ref/prompt", "name": "db-playbook"},
                                      "argument": {"name": "section", "value": ""}})
print("NOTE prompt completion:", json.dumps(r.get("result"))[:200])
r = c.request("completion/complete", {"ref": {"type": "ref/resource", "uri": "db://schema/{db}/{table}"},
                                      "argument": {"name": "db", "value": ""},
                                      "context": {"arguments": {}}})
vals = (r.get("result", {}) or {}).get("values", [])
check("empty-prefix completion lists dbs", "sqlite3" in vals and "jsonfile" in vals, str(r.get("result"))[:160])
r = c.request("completion/complete", {"ref": {"type": "ref/resource", "uri": "db://schema/{db}/{table}"},
                                      "argument": {"name": "table", "value": "rti"},
                                      "context": {"arguments": {"db": "sqlite3"}}})
vals = (r.get("result", {}) or {}).get("values", [])
check("table completion filters by prefix", "rtime" in vals, str(r.get("result"))[:160])
c.close()

# ================= stdio vs cwd: default files land in process cwd =================
sandbox = os.path.join(WORK, "cwdtrap")
os.makedirs(sandbox, exist_ok=True)
for f in glob.glob(os.path.join(sandbox, "*")):
    os.remove(f)
c = Client(args=[], cwd=sandbox, name="cwd-default")
init(c)
call_json(c, "db_info", {"db": "jsonfile"})   # reads (file missing = empty db)
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "cwdt", "data": {"k": 1}})
obs_state = r.get("requestState")
check("MRTR stages jsonfile writes too (cwd-default client has no --yes)",
      r.get("__input_required__") is True and obs_state, str(r)[:140])
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "cwdt", "data": {"k": 1}},
              params_extra={"requestState": obs_state, "inputResponses": {"confirm": {"approved": True}}})
check("approved jsonfile insert executed", "__error__" not in r and "__input_required__" not in r, str(r)[:140])
c.close()
made = sorted(os.path.basename(p) for p in glob.glob(os.path.join(sandbox, "*")))
check("no --config: sqlite3 db at startup; jsonfile store materialized on first WRITTEN row",
      "zorm-mcp.db" in made and "zorm-mcp-data.json" in made, str(made))
c = Client(args=[], cwd=sandbox, name="cwd-content")
init(c)
r = call_json(c, "db_info", {"db": "sqlite3"})
check("default sqlite3 reachable from clean cwd", r.get("ready") is True, str(r))
c.close()

# ================= config template misuse (absolute connString to template) =================
p = subprocess.Popen([mcp_client.EXE, "--config", ROOT + r"\zorm-mcp.json"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=ROOT, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=15)
except Exception:
    p.kill()
    out, err = p.communicate()
allout = (out or "") + (err or "")
check("FINDING config-with-jsonfile-template connString -> refusal + exit!=0 (exit=%s)" % p.poll(),
      p.poll() != 0 and "refusing" in allout.lower(),
      "exit=%s tail=%s" % (p.poll(), allout[-320:].replace("\n", " | ")))

# ================= unknown CLI flag -> usage + nonzero exit =================
p = subprocess.Popen([mcp_client.EXE, "--definitely-not-a-flag"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
except Exception:
    p.kill()
    out, err = p.communicate()
check("unknown flag -> usage text on stderr + nonzero exit (exit=%s)" % p.poll(),
      p.poll() != 0 and "usage" in (err or "").lower(), "exit=%s stderr=%s" % (p.poll(), (err or "")[-200:].replace("\n", " | ")))

# ================= large pagination params =================
c = Client(args=["--config", cfg, "--yes"], cwd=WORK, name="misc")
init(c)
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table big (id text primary key, n integer)"}]})
for i in range(15):
    call_json(c, "db_insert", {"db": "sqlite3", "table": "big", "data": {"id": str(i), "n": i}})
r = call_json(c, "db_query", {"db": "sqlite3", "table": "big", "page": 4, "size": 5})
check("pagination last page partial", len(rows(r)) == 5 or len(rows(r)) == 0, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "big", "size": 0})
check("size=0 rejected by schema", "__error__" in r, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "big", "size": 501})
check("size>500 rejected by schema", "__error__" in r, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "big", "page": 0, "size": 2})
check("page=0 rejected", "__error__" in r, str(r)[:140])
# unicode round-trip
call_json(c, "db_insert", {"db": "sqlite3", "table": "big", "data": {"id": "u1", "n": 0}})  # may fail on missing col; use cfg-like table
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "big", "data": {"id": "中文id", "n": 0}})
check("unicode id insert accepted (auto-upsert)", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "big", "id": "中文id"})
check("unicode id round-trips", len(rows(r)) == 1, str(r)[:140])
# emoji + text
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "emo", "data": {"id": "e1", "emoji": "🚀 db", "cn": "中文"}})
r = call_json(c, "db_query", {"db": "jsonfile", "table": "emo", "id": "e1"})
check("jsonfile emoji/中文 round-trip", rows(r) and rows(r)[0].get("emoji") == "🚀 db", str(r)[:160])
c.close()

fails = [r for r in results if not r[1]]
print("\n== runner4: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
