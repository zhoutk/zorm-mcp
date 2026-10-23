"""Probe 6: jsonfile default-store materialization in a fresh cwd."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK  # noqa: E402

sandbox = os.path.join(WORK, "cwdtrap2")
os.makedirs(sandbox, exist_ok=True)
for f in os.listdir(sandbox):
    os.remove(os.path.join(sandbox, f))
print("before:", os.listdir(sandbox))
c = Client(args=[], cwd=sandbox, name="cwd2")
init(c)
r = call_json(c, "db_info", {"db": "jsonfile"})
print("db_info jsonfile:", str(r)[:160])
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "cwdt", "data": {"k": 1}})
print("insert:", str(r)[:200])
r = call_json(c, "db_insert", {"db": "jsonfile", "table": "cwdt", "data": {"k": 2}})
print("insert2:", str(r)[:200])
c.close()
print("after:", os.listdir(sandbox))
r = call_json(c, "db_query", {"db": "jsonfile", "table": "cwdt"}) if c.p.poll() is None else None
