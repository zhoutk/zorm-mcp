"""Probe: exact envelopes of db_count / db_query_sql counts; jsonfile truncate state."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, text_of, WORK  # noqa: E402

SQL_DB = os.path.join(WORK, "probe-sqlite.db")
JF_DB = os.path.join(WORK, "probe-json.json")
for f in (SQL_DB, JF_DB):
    if os.path.exists(f):
        os.remove(f)
cfg = write_config({"databases": {
    "sqlite3": {"type": "sqlite3", "options": {"connString": SQL_DB, "DbLogClose": True, "parameterized": True}},
    "jsonfile": {"type": "jsonfile", "options": {"connString": JF_DB, "DbLogClose": True}}}})
c = Client(args=["--config", cfg, "--yes"], name="p")
init(c)
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table t (id text primary key, n integer)"}]})
for i in range(3):
    call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": str(i), "n": i}})
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
print("db_count envelope:", json.dumps(r))
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t", "where": {"n": 1}})
print("db_count where envelope:", json.dumps(r))
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select count(*) as n from t"})
print("count sql envelope:", json.dumps(r))
r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "pragma table_info(t)"})
print("pragma raw:", json.dumps(r))
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "jt", "data": {"x": 1}})
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "jt", "data": {"x": 2}})
r = call_json(c, "db_delete", {"db": "jsonfile", "table": "jt", "where": {}})
print("jsonfile delete-all envelope:", json.dumps(r))
r = call_json(c, "db_count", {"db": "jsonfile", "table": "jt"})
print("jsonfile count after delete-all:", json.dumps(r))
r = call_json(c, "db_query", {"db": "jsonfile", "table": "jt"})
print("jsonfile select after delete-all:", json.dumps(r)[:200])
with open(JF_DB, encoding="utf-8") as f:
    print("raw file:", f.read()[:400])
c.close()
