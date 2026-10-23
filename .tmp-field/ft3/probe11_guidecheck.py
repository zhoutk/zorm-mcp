"""Guide-check: verify every expectation written in HANDS-ON-TEST-GUIDE.md §2
against sqlite3 + jsonfile + mysql, exactly as a reader would follow."""
import json
import os
import sys
import re

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
import mcp_client
from mcp_client import Client, init, call_json, WORK  # noqa: E402


def rows(r):
    return (r or {}).get("data") or []


CREATE = {
    "sqlite3": "create table items (id text primary key, name text, price real, qty integer)",
    "jsonfile": None,
    "mysql": "create table items (id varchar(32) primary key, name varchar(64), price double precision, qty int)",
    "postgres": "create table items (id varchar(32) primary key, name varchar(64), price double precision, qty int)",
}
DATA = [("1", "alpha", 9.5, 10), ("2", "beta", 59, 2), ("3", "gamma", 12.25, 10)]

for db in ("sqlite3", "jsonfile", "mysql", "postgres", "dm8"):
    SQL_DB = os.path.join(WORK, "guide-%s.db" % db)
    JF_DB = os.path.join(WORK, "guide-%s.json" % db)
    for f in (SQL_DB, JF_DB, SQL_DB + "-wal", SQL_DB + "-shm"):
        if os.path.exists(f):
            os.remove(f)
    DDL = {
        "sqlite3": CREATE["sqlite3"],
        "mysql": CREATE["mysql"],
        "postgres": CREATE["postgres"],
        "dm8": 'drop table if exists "dbtest"."items"',
    }
    cfg = {"databases": {db: {"type": db, "options": (
        {"connString": SQL_DB, "DbLogClose": True, "parameterized": True} if db == "sqlite3" else
        {"connString": JF_DB, "DbLogClose": True} if db == "jsonfile" else
        {"db_host": "10.0.0.7", "db_port": 3306, "db_name": "dbtest", "db_user": "root", "db_pass": "123456"} if db == "mysql" else
        {"db_host": "10.0.0.7", "db_port": 5432, "db_name": "dbtest", "db_user": "root", "db_pass": "123456"} if db == "postgres" else
        {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest", "db_user": "SYSDBA", "db_pass": "123456"})}}}
    # fresh table on mysql & drop leftovers
    cfg_path = os.path.join(WORK, "gcfg-%s.json" % db)
    with open(cfg_path, "w") as fh:
        json.dump(cfg, fh)
    p = Client(args=["--config", cfg_path, "--yes", "--allow-sql"], name="g-" + db)
    init(p)
    # jsonfile: table materializes on first write; describe BEFORE insert must error
    if db == "jsonfile":
        d0 = call_json(p, "db_describe_table", {"db": db, "table": "items"})
        assert "__error__" in d0 and "unknown table" in d0["message"], str(d0)[:200]
        print("OK %-9s jsonfile no-table-before-write errors as documented" % db)
    elif db == "dm8":
        # guide §2: bare-name CREATE "succeeds" but lands outside dbtest (F-3),
        # qualified quoted-lowercase DDL is the supported path
        call_json(p, "db_exec_sql", {"db": db, "sql": DDL[db]})
        r = call_json(p, "db_exec_sql", {"db": db, "sql": 'create table "dbtest"."items" ("id" varchar(32) primary key, "name" varchar(64), "price" double precision, "qty" integer)'})
        assert "__error__" not in r, "dm8 create: %s" % str(r)
    else:
        call_json(p, "db_exec_sql", {"db": db, "sql": "drop table if exists items"})
        r = call_json(p, "db_exec_sql", {"db": db, "sql": CREATE[db]})
        assert "__error__" not in r, "%s create: %s" % (db, str(r))
    d = call_json(p, "db_describe_table", {"db": db, "table": "items"})
    if db == "jsonfile":
        # before the first write jsonfile has no table; skip the empty check
        pass
    else:
        assert len(d.get("columns", [])) == 4, "%s describe empty: %s" % (db, str(d)[:200])
        pk = [c for c in d["columns"] if c.get("primary_key")]
        assert len(pk) == 1 and pk[0]["name"].lower() == "id", "%s pk: %s" % (db, str(pk))
        print("OK %-9s describe-empty(columns=4, pk=id, types=%s)" % (db, [c["type"] for c in d["columns"]]))
    for i, nm, pr, qt in DATA:
        r = call_json(p, "db_insert", {"db": db, "table": "items", "data": {"id": i, "name": nm, "price": pr, "qty": qt}})
        assert "__error__" not in r and "__unparsed__" not in r, "%s insert: %s" % (db, str(r)[:200])
    cases = []
    cases.append(("eq id=2", call_json(p, "db_query", {"db": db, "table": "items", "id": "2"}),
                  lambda r: [x["id"] for x in rows(r)] == (["2"] if isinstance(rows(r)[0].get("id"), str) else [2])))
    cases.append(("cmp price>,20", call_json(p, "db_query", {"db": db, "table": "items", "price": ">,20"}),
                  lambda r: [str(x["id"]) for x in rows(r)] == ["2"]))
    cases.append(("lks name=a", call_json(p, "db_query", {"db": db, "table": "items", "lks": {"name": "a"}}),
                  lambda r: len(rows(r)) == 3))
    cases.append(("ins qty[10,2]", call_json(p, "db_query", {"db": db, "table": "items", "ins": {"qty": [10, 2]}}),
                  lambda r: len(rows(r)) == 3))
    cases.append(("ors name=beta OR qty=10", call_json(p, "db_query", {"db": db, "table": "items", "ors": {"name": "beta", "qty": 10}}),
                  lambda r: len(rows(r)) == 3))
    cases.append(("fields+sort asc", call_json(p, "db_query", {"db": db, "table": "items", "fields": ["name"], "sort": "price asc"}),
                  lambda r: [x["name"] for x in rows(r)] == ["alpha", "gamma", "beta"]))
    cases.append(("page2/size2", call_json(p, "db_query", {"db": db, "table": "items", "page": 2, "size": 2, "sort": "price asc"}),
                  lambda r: [x["name"] for x in rows(r)] == ["beta"] and r.get("pages") == 2))
    cases.append(("page without size", call_json(p, "db_query", {"db": db, "table": "items", "page": 5}),
                  lambda r: "__error__" in r and "size" in r["message"]))
    cases.append(("group+sum+count", call_json(p, "db_query", {"db": db, "table": "items", "qty": 10, "group": ["qty"], "sum": "price,tp", "count": "*,n"}),
                  lambda r: rows(r) == [{"qty": 10, "tp": 21.75, "n": 2}]))
    for shape in ("*", ["*"], {"*": ""}):
        cases.append(("count %s" % json.dumps(shape), call_json(p, "db_query", {"db": db, "table": "items", "count": shape}),
                      lambda r: rows(r) == [{"total": 3}]))
    cases.append(("count explicit '*,n'", call_json(p, "db_query", {"db": db, "table": "items", "count": "*,n"}),
                  lambda r: rows(r) == [{"n": 3}]))
    cases.append(("F-1 top-level qty", call_json(p, "db_count", {"db": db, "table": "items", "qty": 999}),
                  lambda r: "__error__" in r and "unknown argument 'qty'" in r["message"]))
    cases.append(("count where qty=10", call_json(p, "db_count", {"db": db, "table": "items", "where": {"qty": 10}}),
                  lambda r: rows(r) == [{"total": 2}]))
    bad = 0
    for label, r, ok in cases:
        if not ok(r):
            bad += 1
            print("  MISMATCH [%s] %s -> %s" % (db, label, str(r)[:200]))
    print("OK %-9s %d/%d cases" % (db, len(cases) - bad, len(cases)))
    p.close()
    # post-insert describe for jsonfile (materialized by writes)
    if db == "jsonfile":
        pass
