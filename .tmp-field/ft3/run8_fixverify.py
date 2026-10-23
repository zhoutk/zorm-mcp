"""Regress A: positive verification of the dcefe71 fixes (F-1/F-2/F-4/F-5/F-6/F-10)."""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK, EXE  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:230] if detail else ""))


def rows(r):
    return (r or {}).get("data") or []


SQL_DB = os.path.join(WORK, "fix.db")
for f in (SQL_DB, SQL_DB + "-wal", SQL_DB + "-shm"):
    if os.path.exists(f):
        os.remove(f)
cfg_path = os.path.join(WORK, "fix.json")
with open(cfg_path, "w") as f:
    json.dump({"databases": {"sqlite3": {"type": "sqlite3", "options": {
        "connString": SQL_DB, "DbLogClose": True, "parameterized": True}}}}, f)

c = Client(args=["--config", cfg_path, "--yes", "--allow-sql"], name="fixes")
init(c)
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "create table t (id text primary key, v text, n integer)"}]})
for i, v in [("1", "a"), ("2", "b"), ("3", "c")]:
    call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": i, "v": v, "n": 10}})

# ---- F-1: db_count unknown top-level key -> named rejection (was silent whole-table) ----
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t", "qty": 999})
check("F-1 db_count unknown key rejected by name", "__error__" in r
      and "unknown argument 'qty'" in r["message"] and "where" in r["message"], str(r)[:200])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("F-1 clean db_count still works", rows(r) == [{"total": 3}], str(r)[:120])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t", "where": {"v": "a"}})
check("F-1 db_count with proper where works", rows(r) == [{"total": 1}], str(r)[:120])

# ---- F-2: functional PRAGMA keeps column names (was [{}...]) ----
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma table_info(t)"})
names = [row.get("name") for row in rows(r)]
check("F-2 pragma table_info returns real rows", names == ["id", "v", "n"], str(r)[:200])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma user_version"})
check("F-2 scalar pragma unaffected", "__error__" not in r, str(r)[:120])

# ---- F-4: anonymous '*' count aliases to total; explicit alias untouched ----
for label, cnt in [("string '*'", "*"), ("array ['*']", ["*"]), ("dict {'*':''}", {"*": ""})]:
    r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "count": cnt})
    check("F-4 count %s -> data.total (no syntax error)" % label,
          rows(r) == [{"total": 3}], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "count": {"*": "rows_n"}})
check("F-4 explicit alias {'*':'rows_n'} preserved", rows(r) == [{"rows_n": 3}], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "count": "*,rows_x"})
check("F-4 explicit alias '*,rows_x' preserved", rows(r) == [{"rows_x": 3}], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "count": "n,n_cnt", "group": ["n"]})
check("F-4 regular column count unchanged", rows(r) == [{"n": 10, "n_cnt": 3}], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "sum": ["n"]})
check("F-4 sum array [col] unchanged (alias=col)", rows(r) == [{"n": 30}], str(r)[:140])

# ---- F-10: singular '1 statement' in confirmation text ----
cy = Client(args=["--config", cfg_path], name="f10")  # no --yes
init(cy)
r = call_json(cy, "db_trans", {"db": "sqlite3", "sqls": [{"table": "t", "method": "delete", "params": {}}]})
check("F-10 confirmation says '(1 statement)'", "(1 statement)" in r.get("message", ""), r.get("message", "")[:150])
check("F-10 no '1 statements' remnant", "1 statements" not in r.get("message", ""), r.get("message", "")[:150])
# plural for >1 unchanged
r = call_json(cy, "db_trans", {"db": "sqlite3", "sqls": [
    {"table": "t", "method": "delete", "params": {}},
    {"table": "t", "method": "delete", "params": {}}]})
check("F-10 plural '(2 statements)' unchanged", "(2 statements)" in r.get("message", ""), r.get("message", "")[:150])
cy.close()

# ---- F-9: sqlite error text carries driver detail (submodule 01d8e48) ----
call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "drop table if exists fix_intpk"})
call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "create table fix_intpk (id integer primary key, v text)"})
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "fix_intpk", "data": {"v": "x"}})
check("F-9 datatype mismatch surfaced in error", "__error__" in r and "datatype mismatch" in r["message"],
      str(r)[:200])
call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "drop table fix_intpk"})

# ---- F-5: db_exec_sql description mentions affected-count semantics ----
r = c.request("tools/list", {})
desc = [t.get("description", "") for t in r["result"]["tools"] if t["name"] == "db_exec_sql"][0]
check("F-5 db_exec_sql description documents affected-count semantics",
      "affected row counts" in desc and "NOT result rows" in desc, desc[:180])
c.close()

# ---- F-6: strict numeric parsing for --http / --sse-keepalive ----
p = subprocess.Popen([EXE, "--config", cfg_path, "--http", "bogus"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
except Exception:
    p.kill(); out, err = p.communicate()
check("F-6 --http bogus -> exit 2 + message", p.poll() == 2 and "invalid --http" in (err or ""),
      "exit=%s stderr=%s" % (p.poll(), (err or "")[-160:].replace("\n", " | ")))

p = subprocess.Popen([EXE, "--config", cfg_path, "--sse-keepalive", "bogus"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
except Exception:
    p.kill(); out, err = p.communicate()
check("F-6 --sse-keepalive bogus -> exit 2 + message", p.poll() == 2 and "invalid --sse-keepalive" in (err or ""),
      "exit=%s stderr=%s" % (p.poll(), (err or "")[-160:].replace("\n", " | ")))

# F-6 non-regression: valid values still work (http on ephemeral port + keepalive 300)
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); kp = s.getsockname()[1]; s.close()
p = subprocess.Popen([EXE, "--config", cfg_path, "--http", str(kp), "--sse-keepalive", "300", "--yes"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
time.sleep(1.2)
alive = False
try:
    alive = (p.poll() is None)
    # bounded drain: the keepalive stream pipes small text lines
    import threading
    buf = []
    th = threading.Thread(target=lambda: buf.extend(l for l in p.stderr if True), daemon=True)
    th.start(); th.join(timeout=2.0)
    alive = p.poll() is None and any("HTTP serving" in l for l in buf)
except Exception:
    pass
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()
check("F-6 valid --http/--sse-keepalive still serve", alive, "exit=%s" % p.poll())
p.stderr.close(); p.stdout.close()

fails = [x for x in results if not x[1]]
print("\n== fix-verify: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
