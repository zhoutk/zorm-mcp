"""Probe 7: dm8 real test-table roundtrip + find where CREATE landed."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, approve, WORK  # noqa: E402

cfg = WORK + r"\probe7.json"
import mcp_client
with open(cfg, "w") as f:
    json.dump({"databases": {
        "dm8": {"type": "dm8", "options": {"db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest",
                                           "db_user": "SYSDBA", "db_pass": "123456", "db_conn": 1}}}}, f)
c = Client(args=["--config", cfg, "--yes", "--allow-sql"], name="dm8p")
init(c)
r = call_json(c, "db_list_tables", {"db": "dm8"})
tables = r.get("tables") or []
print("tables:", tables)
for t in tables:
    d = call_json(c, "db_describe_table", {"db": "dm8", "table": t})
    cols = [(col["name"], col["type"], col["primary_key"]) for col in (d.get("columns") or [])]
    cnt = call_json(c, "db_count", {"db": "dm8", "table": t})
    print(t, "->", cols[:6], "count=", (cnt.get("data") or [{}])[0].get("total"))

# did unqualified CREATE land in SYSDBA schema? check via raw catalog select on all_tables
r = call_json(c, "db_query_sql", {"db": "dm8", "sql": "select owner, table_name from all_tables where upper(table_name) = 'FT3_PROBE'"})
print("all_tables FT3_PROBE:", str(r)[:260])
r = call_json(c, "db_query_sql", {"db": "dm8", "sql": "select owner, count(*) n from all_tables where owner in ('DBTEST','SYSDBA') group by owner"})
print("owner counts:", str(r)[:300])

# use existing test table for the write path
if "sec_probe" in tables:
    d = call_json(c, "db_describe_table", {"db": "dm8", "table": "sec_probe"})
    print("sec_probe cols:", json.dumps(d.get("columns"))[:300])
c.close()
