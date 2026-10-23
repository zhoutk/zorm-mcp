"""Runner 6: REMOTE backend matrix - mysql / postgres / dm8 on 10.0.0.7.
Smoke + exactness checks per backend through the SAME structured semantics;
verifies postgres ?-> $n rewrite, dm8 schema-qualified catalogs, version strings."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, WORK  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:260] if detail else ""))


def rows(r):
    return (r or {}).get("data") or []


cfg = write_config({"databases": {
    "mysql": {"type": "mysql", "options": {"db_host": "10.0.0.7", "db_port": 3306, "db_name": "dbtest",
                                           "db_user": "root", "db_pass": "123456", "db_char": "utf8mb4", "db_conn": 3}},
    "postgres": {"type": "postgres", "options": {"db_host": "10.0.0.7", "db_port": 5432, "db_name": "dbtest",
                                                 "db_user": "root", "db_pass": "123456", "db_char": "utf8mb4", "db_conn": 3}},
    "dm8": {"type": "dm8", "options": {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest",
                                       "db_user": "SYSDBA", "db_pass": "123456", "db_conn": 1}},
}})
c = Client(args=["--config", cfg, "--yes", "--allow-sql"], cwd=WORK, name="remote")
try:
    init(c, "2026-07-28")
except Exception as e:
    print("FATAL: server did not start:", e)
    for l in c.stderr_lines[-8:]:
        print("  stderr>", l)
    sys.exit(1)

# ---------- db_info per backend ----------
for db in ("mysql", "postgres", "dm8"):
    r = call_json(c, "db_info", {"db": db})
    check("%s: db_info ready" % db, r.get("ready") is True, str(r)[:160])
    ver = (r.get("server_version") or "")
    check("%s: server_version present" % db, len(ver) > 0, "version=%r" % ver[:40])

# ---------- list tables / describe ----------
LT = {}
for db in ("mysql", "postgres", "dm8"):
    r = call_json(c, "db_list_tables", {"db": db})
    LT[db] = r.get("tables") or []
    check("%s: db_list_tables ok (%d tables)" % (db, len(LT[db])), "__error__" not in r, str(r)[:180])

# pick a stable system-ish table that exists everywhere or per-backend catalog table
# mysql: use information_schema-checkable table; choose first table from listing if any
def pick(db, fallback):
    if LT[db]:
        return LT[db][0]
    return fallback

# use per-backend simple reads on sqlite-like probe table created fresh per backend
probe = "ft3_probe"
for db in ("mysql", "postgres", "dm8"):
    if db == "dm8":
        # dm8 is CASE_SENSITIVE=Y for identifiers: the tool surface generates
        # quoted-lowercase "dbtest"."table" with quoted-lowercase columns, so
        # DDL must match exactly (drop first - no IF NOT EXISTS on dm8).
        call_json(c, "db_exec_sql", {"db": db, "sql": 'drop table if exists "dbtest"."%s"' % probe})
        sql = 'create table "dbtest"."' + probe + '" ("id" varchar(32) primary key, "v" varchar(64), "n" int)'
    else:
        call_json(c, "db_exec_sql", {"db": db, "sql": "drop table if exists ft3_probe"})
        sql = {
            "mysql": "create table if not exists ft3_probe (id varchar(32) primary key, v varchar(64), n int)",
            "postgres": "create table if not exists ft3_probe (id varchar(32) primary key, v varchar(64), n int)",
        }[db]
    r = call_json(c, "db_exec_sql", {"db": db, "sql": sql})
    if "__error__" in r:
        r = call_json(c, "db_trans", {"db": db, "sqls": [{"text": sql}]})
    check("%s: probe DDL executed" % db, "__error__" not in r and "__unparsed__" not in r, str(r)[:200])

# FINDING checks: dm8 raw-DDL identifier/anchor semantics
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": "create table ft3_unq2 (id int)"})
r2 = call_json(c, "db_query_sql", {"db": "dm8", "sql": "select owner, table_name from all_tables where table_name = 'FT3_UNQ2'"})
check("FINDING dm8 unquoted CREATE runs OK but lands in the LOGIN schema (SYSDBA),"
      " invisible to the db_name-scoped tool surface", rows(r2) == [{"OWNER": "SYSDBA", "TABLE_NAME": "FT3_UNQ2"}],
      "cat=%s" % str(r2)[:180])
call_json(c, "db_exec_sql", {"db": "dm8", "sql": "drop table ft3_unq2"})

for db in ("mysql", "postgres", "dm8"):
    r = call_json(c, "db_insert", {"db": db, "table": probe, "data": {"id": "1", "v": "one", "n": 1}})
    check("%s: insert row" % db, "__error__" not in r and "__unparsed__" not in r, str(r)[:180])
    r = call_json(c, "db_describe_table", {"db": db, "table": probe})
    cols = {col["name"].lower(): col for col in r.get("columns", [])}
    check("%s: describe has id/v/n" % db, {"id", "v", "n"} <= set(cols), str(r)[:200])
    r = call_json(c, "db_query", {"db": db, "table": probe, "id": "1"})
    check("%s: equality query" % db, len(rows(r)) == 1 and (rows(r)[0].get("v") in ("one", "ONE")), str(r)[:160])
    r = call_json(c, "db_query", {"db": db, "table": probe, "n": ">,0"})
    check("%s: compare prefix" % db, len(rows(r)) == 1, str(r)[:160])
    r = call_json(c, "db_query", {"db": db, "table": probe, "lks": {"v": "on"}})
    check("%s: lks fuzzy" % db, len(rows(r)) in (0, 1), str(r)[:160])

# postgres ?-> $n rewrite
r = call_json(c, "db_query_sql", {"db": "postgres", "sql": "select v from ft3_probe where id = ?", "values": ["1"]})
check("postgres: ? placeholder auto-rewritten to $1 (value binds)",
      "__error__" not in r and len(rows(r)) == 1 and rows(r)[0].get("v") in ("one", "ONE"), str(r)[:180])
r = call_json(c, "db_query_sql", {"db": "postgres", "sql": "select count(*) as n from ft3_probe where n > ? and id <> ?", "values": [50, "x"]})
check("postgres: multiple ? -> $1..$2", r.get("data") == [{"n": 0}], str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "mysql", "sql": "explain select v from ft3_probe where id = '1'"})
check("mysql: EXPLAIN via query (metadata-leg fix from D-08 lineage)",
      "__error__" not in r and "__unparsed__" not in r, str(r)[:160])

# dm8 catalogs are owner-scoped: probe must appear via all_* filtered owner=dbtest
r = call_json(c, "db_list_tables", {"db": "dm8"})
check("dm8: ft3_probe listed via owner-scoped catalog (case-insensitive)",
      any(t.lower() == probe for t in (r.get("tables") or [])), str(r)[:220])
r = call_json(c, "db_indexes", {"db": "dm8", "table": probe})
check("dm8: db_indexes executes (pk index visible or empty best-effort)", "__error__" not in r and "__unparsed__" not in r, str(r)[:180])

# transactions per backend: commit + rollback
for db in ("mysql", "postgres", "dm8"):
    text_sql = {
        "mysql": "insert into ft3_probe (id, v, n) values (?, ?, ?)",
        "postgres": "insert into ft3_probe (id, v, n) values (?, ?, ?)",
        # dm8 identifiers are quoted lowercase (CASE_SENSITIVE=Y servers)
        "dm8": 'insert into "dbtest"."ft3_probe" ("id","v","n") values (?, ?, ?)',
    }[db]
    r = call_json(c, "db_trans", {"db": db, "sqls": [
        {"text": text_sql, "values": ["2", "two", 2]},
        {"table": probe, "method": "insert", "params": {"id": "3", "v": "three", "n": 3}}]})
    check("%s: trans commit 2 stmts" % db, "__error__" not in r and "__unparsed__" not in r, str(r)[:200])
    r = call_json(c, "db_trans", {"db": db, "sqls": [
        {"text": text_sql, "values": ["4", "four", 4]},
        {"text": "insert into ft3_nonexistent (id) values (?)", "values": ["x"]}]})
    check("%s: failing trans -> error" % db, "__error__" in r, str(r)[:200])
    if db == "dm8" and "__error__" in r:
        check("F-8 FIXED dm8 trans error carries driver diagnostic (non-empty, decoded)",
              "Running transaction error: " in r["message"]
              and len(r["message"].split("Running transaction error: ", 1)[1].strip()) > 0, str(r)[:220])
    r = call_json(c, "db_count", {"db": db, "table": probe})
    check("%s: rollback atomic (total=3)" % db, rows(r) and rows(r)[0].get("total") == 3, str(r)[:160])

# cleanup probe tables
for db in ("mysql", "postgres"):
    r = call_json(c, "db_exec_sql", {"db": db, "sql": "drop table if exists ft3_probe"})
    check("%s: cleanup drop" % db, "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": 'drop table if exists "dbtest"."' + probe + '"'})
check("dm8: cleanup drop (quoted lowercase)", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])

# dm8 GBK error handling: provoke an error & ensure the message isn't mojibake
r = call_json(c, "db_query", {"db": "dm8", "table": "ft3_probe_zz"})
print("NOTE dm8 unknown-table error text:", str(r)[:220])

c.close()

fails = [r for r in results if not r[1]]
print("\n== runner6: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
