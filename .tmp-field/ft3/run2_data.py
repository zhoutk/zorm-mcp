"""Runner 2 (v2): data-plane field tests on sqlite3 + jsonfile (--yes writes).
Fixed: TEXT pk for zorm's 8-hex auto ids; ors values are comma-free pairs;
compare prefixes live on top-level column conditions only."""
import json
import os
import sys
import re

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, tool_names, WORK  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:240] if detail else ""))


def rows_of(r):
    if isinstance(r, dict):
        return r.get("data") or []
    return []


SQL_DB = os.path.join(WORK, "db2-sqlite.db")
JF_DB = os.path.join(WORK, "db2-json.json")
for f in (SQL_DB, JF_DB):
    if os.path.exists(f):
        os.remove(f)

cfg = write_config({"databases": {
    "sqlite3": {"type": "sqlite3", "options": {"connString": SQL_DB, "DbLogClose": True, "parameterized": True}},
    "jsonfile": {"type": "jsonfile", "options": {"connString": JF_DB, "DbLogClose": True}},
}})
c = Client(args=["--config", cfg, "--yes"], name="data")
init(c, "2026-07-28")

check("12 tools under --yes (db_exec_sql hidden)", tool_names(c) == [
    "db_count", "db_delete", "db_describe_table", "db_indexes", "db_info",
    "db_insert", "db_insert_batch", "db_list_tables", "db_query",
    "db_query_sql", "db_trans", "db_update"], str(tool_names(c)))

for db in ("sqlite3", "jsonfile"):
    r = call_json(c, "db_list_tables", {"db": db})
    check("%s: empty db lists 0 tables" % db, r.get("tables") == [] and r.get("count") == 0, str(r))

# ============ sqlite3 ============
r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "create table items (id text primary key, name text, price real, qty integer)"}]})
check("sqlite: CREATE TABLE via db_trans", "__error__" not in r and "__unparsed__" not in r, str(r)[:160])
check("sqlite: trans returns success envelope", r.get("status") == 200, str(r)[:100])
r = call_json(c, "db_list_tables", {"db": "sqlite3"})
check("sqlite: table now listed", "items" in r.get("tables", []), str(r))
r = call_json(c, "db_describe_table", {"db": "sqlite3", "table": "items"})
cols = {col["name"]: col for col in r.get("columns", [])}
check("sqlite: describe on EMPTY table (catalog)", set(cols) == {"id", "name", "price", "qty"}, str(r)[:200])
check("sqlite: id pk", cols.get("id", {}).get("primary_key") is True, str(cols.get("id")))

for i, nm, pr, qt in [(1, "alpha", 9.5, 10), (2, "beta", 59.0, 2), (3, "gamma", 12.25, 10)]:
    call_json(c, "db_insert", {"db": "sqlite3", "table": "items",
                               "data": {"id": str(i), "name": nm, "price": pr, "qty": qt}})
check("sqlite: 3 explicit inserts",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 3)

r = call_json(c, "db_insert", {"db": "sqlite3", "table": "items",
                               "data": {"name": "delta", "price": 1.0, "qty": 0}})
aid = r.get("id")
check("sqlite: missing id auto-generated (8-hex string)",
      "__error__" not in r and isinstance(aid, str) and re.fullmatch(r"[0-9a-fA-F]{8}", aid) is not None, str(r)[:160])
check("sqlite: count=4 after auto-id",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 4)

r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "id": "2"})
check("sqlite: equality cond", [x["id"] for x in rows_of(r)] == ["2"], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "price": ">,20"})
check("sqlite: compare prefix price>20", [x["id"] for x in rows_of(r)] == ["2"], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "lks": {"name": "a"}})
check("sqlite: lks substring", sorted(x["id"] for x in rows_of(r)) == sorted(["1", "2", "3", aid]), str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "ins": {"qty": [10, 2]}})
check("sqlite: ins multi-value", sorted(x["id"] for x in rows_of(r)) == ["1", "2", "3"], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "ors": {"name": "beta", "qty": 0}})
check("sqlite: ors (name=beta OR qty=0)", sorted(x["id"] for x in rows_of(r)) == ["2", aid], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "ors": {"price": ">,5", "qty": "10"}})
check("sqlite: compare prefix inside ors rejected with fix hint",
      "__error__" in r and "db_query_sql" in r["message"], str(r)[:200])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "fields": ["name"], "sort": "price asc"})
check("sqlite: projection+sort(price asc)", [x.get("name") for x in rows_of(r)] == ["delta", "alpha", "gamma", "beta"], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "fields": ["id", "price"], "sort": "price desc"})
check("sqlite: sort desc", [x.get("id") for x in rows_of(r)] == ["2", "3", "1", aid], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "size": 2, "sort": "price asc"})
check("sqlite: size alone -> page 1", [x.get("name") for x in rows_of(r)] == ["delta", "alpha"], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "page": 2, "size": 2, "sort": "price asc"})
check("sqlite: page 2 of 2", [x.get("name") for x in rows_of(r)] == ["gamma", "beta"] and r.get("pages") == 2, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "page": 5})
check("sqlite: page without size -> explicit error", "__error__" in r and "size" in r["message"], str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "qty": 10, "group": ["qty"], "sum": "price,tp", "count": "*,n"})
check("sqlite: group+sum+count aggregate", rows_of(r) == [{"qty": 10, "tp": 21.75, "n": 2}], str(r)[:200])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "fields": ["id"], "id": "nope"})
check("sqlite: empty result shape", r.get("status") in (200, 202) and r.get("data") == [], str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "sum": {"price": "p"}})
check("sqlite: sum {col:alias}", abs(rows_of(r)[0]["p"] - 81.75) < 1e-9, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "sum": ["price"], "count": []})
check("sqlite: sum array form [col] (alias=col); count 'col,alias'-only shapes", "__error__" not in r and "__unparsed__" not in r,
      str(r)[:170])
print("NOTE sum array form returns:", str(r)[:170])
# F-1 FIXED (dcefe71): unknown top-level key now rejected by name
r = call_json(c, "db_count", {"db": "sqlite3", "table": "items", "qty": 999})
check("F-1 FIXED db_count unknown top-level key -> named InvalidParams",
      "__error__" in r and "unknown argument 'qty'" in r["message"] and "where" in r["message"], str(r)[:180])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "bogus_key": 1})
check("sqlite: db_query rejects unknown top-level keys", "__error__" in r, str(r)[:170])

# identifier validation (pre-SQL, invalid params)
for label, tool, args, pat in [
    ("table with space", "db_query", {"db": "sqlite3", "table": "it ems", "id": "1"}, "invalid"),
    ("lks key with quote", "db_query", {"db": "sqlite3", "table": "items", "lks": {"na'me": "x"}}, "invalid"),
    ("sort injection", "db_query", {"db": "sqlite3", "table": "items", "sort": "id; drop table items"}, "invalid"),
    ("numeric-leading field", "db_query", {"db": "sqlite3", "table": "items", "fields": ["1abc"]}, "invalid"),
    ("data key with space", "db_update", {"db": "sqlite3", "table": "items", "data": {"na me": 1}, "where": {"id": "2"}}, "invalid"),
    ("dotted table", "db_insert", {"db": "sqlite3", "table": "main.items", "data": {"id": "9"}}, "invalid"),
    ("group with quote", "db_query", {"db": "sqlite3", "table": "items", "group": ["qty'; --"]}, "invalid"),
    ("count alias injection", "db_query", {"db": "sqlite3", "table": "items", "count": "*,tot'al"}, "invalid"),
]:
    r = call_json(c, tool, args)
    ok = "__error__" in r and (pat in r["message"].lower() or "name" in r["message"].lower())
    check("sqlite: %s rejected pre-SQL" % label, ok, str(r)[:170])

r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "lks": {"name": "a,b"}})
check("sqlite: comma inside lks value -> guided to db_query_sql",
      "__error__" in r and "db_query_sql" in r["message"], str(r)[:200])

# update / delete
r = call_json(c, "db_update", {"db": "sqlite3", "table": "items", "data": {"qty": 3}, "where": {"id": "2"}})
check("sqlite: update by id", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
row = rows_of(call_json(c, "db_query", {"db": "sqlite3", "table": "items", "id": "2"}))[0]
check("sqlite: update applied", row["qty"] == 3 and row["name"] == "beta", str(row))
r = call_json(c, "db_update", {"db": "sqlite3", "table": "items", "data": {"qty": 3}, "where": {"name": "beta"}})
check("sqlite: update where without id -> error", "__error__" in r and "id" in r["message"], str(r)[:160])
r = call_json(c, "db_delete", {"db": "sqlite3", "table": "items", "where": {"id": "3"}})
check("sqlite: delete by id", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
check("sqlite: count=3 after delete",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 3)
r = call_json(c, "db_delete", {"db": "sqlite3", "table": "items", "where": {"qty": 3}})
check("sqlite: conditional delete via where:{id..} only -> explicit db_trans hint",
      "__error__" in r and "db_trans" in r["message"], str(r)[:220])
check("sqlite: count=3 after id-delete (conditional delete was rejected, no rows lost)",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 3)

r = call_json(c, "db_insert_batch", {"db": "sqlite3", "table": "items", "rows": [
    {"id": "2", "name": "beta2", "price": 2.0, "qty": 9},
    {"id": "5", "name": "eps", "price": 3.0, "qty": 1}]})
check("sqlite: insert_batch upsert", "__error__" not in r and "__unparsed__" not in r, str(r)[:180])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "items", "sort": "price asc"})
check("sqlite: upsert replaced id=2, added id=5 (no single insertId)",
      [x["name"] for x in rows_of(r)] == ["delta", "beta2", "eps", "alpha"]
      and "insertId" not in r, str(r)[:180])

r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "insert into items (id, name, price, qty) values (?, ?, ?, ?)", "values": ["10", "t1", 5.0, 1]},
    {"table": "items", "method": "insert", "params": {"id": "11", "name": "t2", "price": 6.0, "qty": 1}}]})
check("sqlite: trans mixed text+structured", "__error__" not in r and "__unparsed__" not in r, str(r)[:180])
check("sqlite: trans committed 2 rows (count=6)",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 6)
r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "insert into items (id, name) values (?, ?)", "values": ["12", "rollback"]},
    {"text": "insert into nosuch_table (id) values (?)", "values": [1]}]})
check("sqlite: failing trans -> error", "__error__" in r, str(r)[:200])
check("sqlite: trans rolled back atomically (count still 6)",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 6)

# db_query_sql
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select name, price from items where price > ? order by price desc", "values": [5.0]})
check("sqlite: parameterized read", [x["name"] for x in rows_of(r)] == ["alpha", "t2"], str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"})
check("sqlite: aggregate via read gate", rows_of(r)[0]["n"] == 6, str(r)[:140])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "-- probe\nselect 1 as x /* trailing */"})
check("sqlite: comments around SELECT", rows_of(r) == [{"x": 1}], str(r)[:140])
# F-2 FIXED (zorm 01d8e48): functional PRAGMA keeps column names
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma table_info(items)"})
check("F-2 FIXED pragma table_info returns named rows via the gate",
      "__error__" not in r and rows_of(r) and rows_of(r)[0].get("name") == "id", str(r)[:180])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select name, type from pragma_table_info('items')"})
check("sqlite: TVF pragma_table_info works through the gate (documented workaround)",
      rows_of(r) and rows_of(r)[0].get("name") == "id", str(r)[:180])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma user_version"})
check("sqlite: PRAGMA read ok", "__error__" not in r, str(r)[:140])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma user_version = 42"})
check("sqlite: PRAGMA assignment rejected", "__error__" in r and "READ-ONLY" in r["message"], str(r)[:200])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma journal_mode(name)"})
check("sqlite: PRAGMA name(value) assignment rejected", "__error__" in r, str(r)[:200])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "with c as (select 1) delete from items"})
check("sqlite: WITH..DELETE rejected", "__error__" in r and "READ-ONLY" in r["message"], str(r)[:200])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "update items set qty = 0"})
check("sqlite: UPDATE rejected", "__error__" in r and "READ-ONLY" in r["message"], str(r)[:200])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select 1; select 2"})
check("sqlite: multi-statement rejected", "__error__" in r, str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select * from items; drop table items"})
check("sqlite: trailing-injection rejected", "__error__" in r, str(r)[:160])
check("sqlite: items survived gate attacks (count=6)",
      call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"}).get("data", [{}])[0].get("n") == 6)
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select 1.5 as f, 2 as i"})
check("sqlite: numbers stay JSON numbers", isinstance(rows_of(r)[0].get("f"), (int, float)), str(r)[:140])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "explain query plan select name from items"})
check("sqlite: EXPLAIN SELECT ok", "__error__" not in r, str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "explain analyze create table zz as select * from items"})
check("sqlite: EXPLAIN ANALYZE CREATE rejected", "__error__" in r and "EXPLAIN" in r["message"], str(r)[:220])

# self-correcting errors
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "nosuch", "data": {"id": "1"}})
check("sqlite: insert unknown table errors", "__error__" in r, str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "nosuch", "id": "1"})
check("sqlite: query unknown table errors (driver text)", "__error__" in r and "no such table" in r["message"].lower(), str(r)[:160])
r = call_json(c, "db_describe_table", {"db": "sqlite3", "table": "nosuch"})
check("sqlite: describe unknown table -> db_list_tables hint", "__error__" in r and "db_list_tables" in r["message"], str(r)[:160])
r = call_json(c, "db_indexes", {"db": "sqlite3", "table": "items"})
check("sqlite: db_indexes on items", "__error__" not in r and "__unparsed__" not in r, str(r)[:200])

# ============ jsonfile ============
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "items",
                               "data": {"name": "alpha", "price": 9.5, "qty": 10}})
check("jsonfile: auto id insert", "__error__" not in r and "__unparsed__" not in r, str(r)[:160])
call_json(c, "db_insert", {"db": "jsonfile", "table": "items", "data": {"id": 2, "name": "beta", "price": 59.0, "qty": 2}})
call_json(c, "db_insert", {"db": "jsonfile", "table": "items", "data": {"id": 3, "name": "gamma", "price": 12.25, "qty": 10}})
r = call_json(c, "db_list_tables", {"db": "jsonfile"})
check("jsonfile: table appears after write", "items" in r.get("tables", []), str(r))
r = call_json(c, "db_describe_table", {"db": "jsonfile", "table": "items"})
cols = {col["name"]: col for col in r.get("columns", [])}
check("jsonfile: describe columns", {"id", "name", "price", "qty"} <= set(cols), str(r)[:220])
check("jsonfile: id pk", cols.get("id", {}).get("primary_key") is True, str(cols.get("id")))

r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "id": "2"})
check("jsonfile: weak typing string '2' hits int 2", [x["id"] for x in rows_of(r)] == [2], str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "price": ">,20"})
check("jsonfile: compare prefix", [x["id"] for x in rows_of(r)] == [2], str(r)[:140])
print("NOTE jsonfile compare result:", str(r)[:170])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "lks": {"name": "a"}})
check("jsonfile: lks substring", len(rows_of(r)) == 3, str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "ins": {"qty": [10, 2]}})
check("jsonfile: ins multi-value", len(rows_of(r)) == 3, str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "ors": {"name": "beta", "qty": 2}})
check("jsonfile: ors", len(rows_of(r)) == 1 and rows_of(r)[0]["name"] == "beta", str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "fields": ["name"], "sort": "price asc"})
check("jsonfile: projection+sort", [x.get("name") for x in rows_of(r)] == ["alpha", "gamma", "beta"], str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "page": 2, "size": 2, "sort": "price asc"})
check("jsonfile: page 2", [x.get("name") for x in rows_of(r)] == ["beta"], str(r)[:140])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "qty": 10, "group": ["qty"], "sum": "price,tp", "count": "*,n"})
check("jsonfile: group+sum+count", rows_of(r) and abs(rows_of(r)[0]["tp"] - 21.75) < 1e-9 and rows_of(r)[0]["n"] == 2, str(r)[:200])
r = call_json(c, "db_update", {"db": "jsonfile", "table": "items", "data": {"qty": 3}, "where": {"id": 2}})
check("jsonfile: update by id", "__error__" not in r and "__unparsed__" not in r, str(r)[:140])
row = rows_of(call_json(c, "db_query", {"db": "jsonfile", "table": "items", "id": 2}))[0]
check("jsonfile: update applied", row["qty"] == 3, str(row))
r = call_json(c, "db_insert_batch", {"db": "jsonfile", "table": "items", "rows": [
    {"id": 2, "name": "beta2", "price": 1.0, "qty": 9},
    {"id": 5, "name": "eps", "price": 3.0, "qty": 1}]})
check("jsonfile: insert_batch upsert", "__error__" not in r and "__unparsed__" not in r, str(r)[:160])
r = call_json(c, "db_query", {"db": "jsonfile", "table": "items", "sort": "price asc"})
check("jsonfile: upsert replaced id=2, added id=5",
      [x["name"] for x in rows_of(r)] == ["beta2", "eps", "alpha", "gamma"], str(r)[:180])

# raw SQL subset: select..from only
r = call_json(c, "db_query_sql", {"db": "jsonfile", "sql": "select name from items"})
check("jsonfile: select..from allowed", "__error__" not in r and len(rows_of(r)) == 4, str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "jsonfile", "sql": "select name from items where id = 2"})
check("jsonfile: WHERE variant rejected with db_query hint", "__error__" in r and ("db_query" in r["message"]), str(r)[:220])
r = call_json(c, "db_query_sql", {"db": "jsonfile", "sql": "select count(*) from items"})
check("jsonfile: aggregates rejected", "__error__" in r, str(r)[:160])
r = call_json(c, "db_query_sql", {"db": "jsonfile", "sql": "select x from nothing_here"})
check("jsonfile: unknown table rejected", "__error__" in r, str(r)[:160])

r = call_json(c, "db_trans", {"db": "jsonfile", "sqls": [{"text": "select 1"}]})
print("NOTE jsonfile trans select-only:", str(r)[:180])

r = call_json(c, "db_delete", {"db": "jsonfile", "table": "items", "where": {}})
check("jsonfile: full-table delete (where:{})", "__error__" not in r and "__unparsed__" not in r, str(r)[:200])
r = call_json(c, "db_list_tables", {"db": "jsonfile"})
check("jsonfile: empty table kept in storage (listed, count total=0)",
      "items" in r.get("tables", []) and call_json(c, "db_count", {"db": "jsonfile", "table": "items"}).get("data", [{}])[0].get("total") == 0, str(r))

# persistence across restart
call_json(c, "db_insert", {"db": "jsonfile", "table": "persist", "data": {"k": "v1"}})
call_json(c, "db_insert", {"db": "sqlite3", "table": "persist_sq", "data": {"id": "1", "k": "v2"}})
c.close()
c = Client(args=["--config", cfg, "--yes"], name="data2")
init(c, "2026-07-28")
r = call_json(c, "db_query", {"db": "jsonfile", "table": "persist"})
check("jsonfile: data persists across restart", len(rows_of(r)) == 1, str(r)[:140])
r = call_json(c, "db_list_tables", {"db": "sqlite3"})
check("sqlite: data persists across restart", "items" in r.get("tables", []), str(r))
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from items"})
check("sqlite: row count persists across restart (n=6)", rows_of(r) and rows_of(r)[0].get("n") == 6, str(r)[:140])
r = call_json(c, "db_describe_table", {"db": "sqlite3", "table": "items"})
check("sqlite: schema recaches after restart", len(r.get("columns", [])) == 4, str(r)[:140])
r = call_json(c, "db_list_tables", {"db": "jsonfile"})
check("jsonfile: multiple tables listed", set(r.get("tables", [])) >= {"items", "persist"}, str(r))
c.close()

fails = [r for r in results if not r[1]]
print("\n== runner2: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
