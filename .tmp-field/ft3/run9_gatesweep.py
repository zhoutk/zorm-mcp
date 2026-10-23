"""Regress B: gate regression sweep after dcefe71 - all historical bypasses
must still be rejected, and the fixes must not have opened new ones."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:230] if detail else ""))


def rows(r):
    return (r or {}).get("data") or []


SQL_DB = os.path.join(WORK, "gatefix.db")
for f in (SQL_DB, SQL_DB + "-wal", SQL_DB + "-shm"):
    if os.path.exists(f):
        os.remove(f)
cfg_path = os.path.join(WORK, "gatefix.json")
with open(cfg_path, "w") as f:
    json.dump({"databases": {"sqlite3": {"type": "sqlite3", "options": {
        "connString": SQL_DB, "DbLogClose": True, "parameterized": True}}}}, f)

c = Client(args=["--config", cfg_path, "--yes", "--allow-sql"], name="gate")
init(c)
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "create table t (id text primary key, v text)"}]})
for i in range(3):
    call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": str(i), "v": "x"}})

REJECT = [
    # statement-level DML
    "update t set v = 'pwned'",
    "delete from t",
    "insert into t (id) values ('z')",
    "drop table t",
    "alter table t add column k text",
    # CTE-borne DML
    "with c as (select 1) delete from t",
    "with c as (select 1) update t set v = 'x'",
    "with c as (select 1) insert into t select '1', 'x'",
    # EXPLAIN wrapping executable non-read
    "explain analyze create table gateprobe as select * from t",
    "explain create table gateprobe as select * from t",
    # PRAGMA writes
    "pragma user_version = 42",
    "pragma journal_mode = wal",
    "pragma writable_schema = on",
]
for sql in REJECT:
    r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": sql})
    check("gate rejects: %.58s" % sql, "__error__" in r, str(r)[:140])

ALLOW = [
    "select * from t",
    "with c as (select 1) select * from c",
    "select count(*) as n from t",
    "pragma user_version",
    "pragma table_info(t)",
    "select name, type from pragma_table_info('t')",
    "explain select v from t",
    "explain query plan select v from t",
]
for sql in ALLOW:
    r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": sql})
    check("gate allows: %.58s" % sql, "__error__" not in r, str(r)[:140])

# F-6 family: db_exec_sql still bypasses by design (operator opt-in)
r = call_json(c, "db_exec_sql", {"db": "sqlite3", "sql": "pragma user_version = 42"})
check("escape hatch db_exec_sql still writes by design", r.get("status") == 200, str(r)[:120])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma user_version"})
check("and the gate still reads the persisted write", rows(r) == [{"user_version": 42}], str(r)[:120])

# integrity after the sweep
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t", "where": {"v": "pwned"}})
check("table untouched by all rejected vectors (pwned absent)", rows(r) == [{"total": 0}], str(r)[:120])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("row count intact", rows(r) == [{"total": 3}], str(r)[:120])
c.close()

fails = [x for x in results if not x[1]]
print("\n== gate-sweep: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
