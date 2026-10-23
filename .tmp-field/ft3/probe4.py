"""Inspect final mrtr.db contents to ground-truth the MRTR run."""
import os
import sys
import json

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, WORK  # noqa: E402

cfg = write_config({"databases": {"sqlite3": {"type": "sqlite3", "options": {
    "connString": os.path.join(WORK, "mrtr.db"), "DbLogClose": True, "parameterized": True}}}})
boot = Client(args=["--config", cfg, "--yes"], name="inspect")
init(boot)
r = call_json(boot, "db_query", {"db": "sqlite3", "table": "t", "sort": "id asc"})
print(json.dumps(r.get("data"), indent=None))
boot.close()
