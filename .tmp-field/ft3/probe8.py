"""Probe 8: where did qualified dm8 create land; owner of existing lowercase tables."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK  # noqa: E402

cfg = WORK + r"\probe8.json"
with open(cfg, "w") as f:
    json.dump({"databases": {
        "dm8": {"type": "dm8", "options": {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest",
                                           "db_user": "SYSDBA", "db_pass": "123456", "db_conn": 1}}}}, f)
c = Client(args=["--config", cfg, "--yes", "--allow-sql"], name="dm8p8")
init(c)
qs = [
    "select owner, table_name from all_tables where lower(table_name) = 'ft3_probe'",
    "select owner, table_name from all_tables where lower(table_name) = 'ft3_unqual_probe'",
    "select owner, table_name from all_tables where table_name = 'sec_probe' collate ... ",
]
for q in qs[0:2]:
    r = call_json(c, "db_query_sql", {"db": "dm8", "sql": q})
    print(q[:80], "->", str(r)[:220])
# creation with quoted lowercase schema as zorm generates for DML:
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": 'drop table if exists "dbtest"."ft3_probe"'})
print("drop quoted-lower:", str(r)[:160])
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": 'create table "dbtest"."ft3_probe" (id varchar(32) primary key, v varchar(64), n int)'})
print("create quoted-lower:", str(r)[:160])
r = call_json(c, "db_insert", {"db": "dm8", "table": "ft3_probe", "data": {"id": "1", "v": "one", "n": 1}})
print("insert via tool:", str(r)[:200])
r = call_json(c, "db_query", {"db": "dm8", "table": "ft3_probe", "id": "1"})
print("query via tool:", str(r)[:200])
r = call_json(c, "db_list_tables", {"db": "dm8"})
print("list:", str(r)[:260])
r = call_json(c, "db_count", {"db": "dm8", "table": "ft3_probe"})
print("count:", str(r)[:180])
r = call_json(c, "db_indexes", {"db": "dm8", "table": "ft3_probe"})
print("indexes:", str(r)[:200])
c.close()
