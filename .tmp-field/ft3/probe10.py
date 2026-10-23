"""Probe 10: leftover coverage gaps - jsonfile db_indexes, dm8 index info, accumulate final stats."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK  # noqa: E402

cfg = WORK + r"\probe10.json"
JF = WORK + r"\p10.json"
if os.path.exists(JF):
    os.remove(JF)
with open(cfg, "w") as f:
    json.dump({"databases": {"jsonfile": {"type": "jsonfile", "options": {"connString": JF, "DbLogClose": True}},
                             "dm8": {"type": "dm8", "options": {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest",
                                                               "db_user": "SYSDBA", "db_pass": "123456", "db_conn": 1}},
                             "mysql": {"type": "mysql", "options": {"db_host": "10.0.0.7", "db_port": 3306, "db_name": "dbtest",
                                                                    "db_user": "root", "db_pass": "123456", "db_conn": 3}}}}, f)
c = Client(args=["--config", cfg, "--yes"], name="p10")
init(c)
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "t", "data": {"id": "1", "x": 1}})
r = call_json(c, "db_indexes", {"db": "jsonfile", "table": "t"})
print("jsonfile db_indexes:", str(r)[:200])
r = call_json(c, "db_describe_table", {"db": "jsonfile", "table": "__nope__"})
print("jsonfile describe unknown:", str(r)[:160])
r = call_json(c, "db_trans", {"db": "jsonfile", "sqls": [{"table": "t", "method": "insert", "params": {"id": "2", "x": 2}}]})
print("jsonfile trans structured:", str(r)[:200])
r = call_json(c, "db_count", {"db": "jsonfile", "table": "t"})
print("jsonfile count:", str(r)[:140])
c.close()

c = Client(args=["--config", cfg, "--yes"], name="p10b")
init(c)
r = call_json(c, "db_indexes", {"db": "mysql", "table": "sec_probe"})
print("mysql db_indexes:", str(r)[:240])
c.close()
