"""Probe 3: sqlite PRAGMA variants through the read-only gate."""
import json
import os
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, WORK  # noqa: E402

SQL_DB = os.path.join(WORK, "probe3-sqlite.db")
if os.path.exists(SQL_DB):
    os.remove(SQL_DB)
cfg = write_config({"databases": {"sqlite3": {"type": "sqlite3", "options": {
    "connString": SQL_DB, "DbLogClose": True, "parameterized": True}}}})
c = Client(args=["--config", cfg, "--yes"], name="p3")
init(c)
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "create table t (id text primary key, n integer)"}]})
for sql in [
    "pragma table_info(t)",
    "select * from pragma_table_info('t') as pti",
    "pragma mmap_size",
    "select name, type from pragma_table_info('t') where pk = 1",
    "PRAGMA TABLE_INFO ('t')",
]:
    r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": sql})
    data = r.get("data")
    print("SQL> %s\n  -> %s" % (sql, (json.dumps(data)[:220] if not data is None else str(r)[:220])))
c.close()
