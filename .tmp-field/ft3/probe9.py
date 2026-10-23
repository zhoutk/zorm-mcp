"""Probe 9: dm8 GBK error conversion stability across calls/sessions."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK  # noqa: E402

cfg = WORK + r"\probe9.json"
with open(cfg, "w") as f:
    json.dump({"databases": {
        "dm8": {"type": "dm8", "options": {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest",
                                           "db_user": "SYSDBA", "db_pass": "123456", "db_conn": 1}}}}, f)
c = Client(args=["--config", cfg, "--yes", "--allow-sql"], name="gbk")
init(c)
for i in range(4):
    r = call_json(c, "db_query", {"db": "dm8", "table": "zzz_missing_%d" % i})
    msg = r.get("message", "")
    print(i, "->", repr(msg[:120]))

# trans variants for mixed-mode test
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": 'drop table if exists "dbtest"."gbk_probe"'})
r = call_json(c, "db_exec_sql", {"db": "dm8", "sql": 'create table "dbtest"."gbk_probe" ("id" varchar(32) primary key, "v" varchar(64))'})
print("create:", str(r)[:100])
r = call_json(c, "db_trans", {"db": "dm8", "sqls": [
    {"text": 'insert into "dbtest"."gbk_probe" ("id","v") values (?, ?)', "values": ["t1", "a"]},
    {"text": 'insert into "dbtest"."gbk_probe" ("id","v") values (?, ?)', "values": ["t2", "b"]}]})
print("trans text-only:", str(r)[:160])
r = call_json(c, "db_count", {"db": "dm8", "table": "gbk_probe"})
print("count after text-only trans:", str(r)[:120])
r = call_json(c, "db_trans", {"db": "dm8", "sqls": [
    {"table": "gbk_probe", "method": "insert", "params": {"id": "s1", "v": "x"}},
    {"table": "gbk_probe", "method": "insert", "params": {"id": "s2", "v": "y"}}]})
print("trans structured-only:", str(r)[:160])
r = call_json(c, "db_count", {"db": "dm8", "table": "gbk_probe"})
print("count after structured trans:", str(r)[:120])
r = call_json(c, "db_trans", {"db": "dm8", "sqls": [
    {"text": 'insert into "dbtest"."gbk_probe" ("id","v") values (?, ?)', "values": ["m1", "z"]},
    {"table": "gbk_probe", "method": "insert", "params": {"id": "m2", "v": "w"}}]})
print("trans MIXED:", str(r)[:200])
r = call_json(c, "db_count", {"db": "dm8", "table": "gbk_probe"})
print("count after mixed trans:", str(r)[:120])
r = call_json(c, "db_query_sql", {"db": "dm8", "sql": "drop table if exists \"dbtest\".\"gbk_probe\""})
c.close()
# fresh session repeat of the same failing query
c = Client(args=["--config", cfg, "--yes"], name="gbk2")
init(c)
r = call_json(c, "db_query", {"db": "dm8", "table": "zzz_missing_fresh"})
print("fresh ->", repr((r.get("message") or "")[:120]))
r = call_json(c, "db_query", {"db": "dm8", "table": "zzz_missing_2again"})
print("fresh2 ->", repr((r.get("message") or "")[:120]))
c.close()
