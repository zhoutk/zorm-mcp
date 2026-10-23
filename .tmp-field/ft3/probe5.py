"""Probe 5: notification timing/flush, config-misuse & bad-flag stderr drain."""
import json
import os
import sys
import time

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, WORK, ROOT, subprocess, text_of  # noqa: E402

cfg = write_config({"databases": {
    "sqlite3": {"type": "sqlite3", "options": {"connString": os.path.join(WORK, "probe5.db"), "DbLogClose": True, "parameterized": True}},
    "jsonfile": {"type": "jsonfile", "options": {"connString": os.path.join(WORK, "probe5.json"), "DbLogClose": True}}}})

c = Client(args=["--config", cfg, "--yes"], name="notif")
init(c)
# does resources/subscribe exist as a method at all?
for m, p in [("resources/subscribe", {"uri": "db://schema/sqlite3"}),
             ("resources/unsubscribe", {"uri": "db://schema/sqlite3"})]:
    r = c.request(m, p)
    print(m, "->", json.dumps(r)[:180])
call_json(c, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table nt (id text primary key)"}]})
r = c.request("ping", {})   # force a flush
print("after write: notifs:", json.dumps(c.notif_methods()))
call_json(c, "db_insert", {"db": "sqlite3", "table": "nt", "data": {"id": "1"}})
r = c.request("ping", {})
print("after insert: notifs:", json.dumps(c.notif_methods()))
c.wait_notifs(2.0)
print("after wait: notifs:", json.dumps(c.notif_methods()))
if "notifications/resources/updated" in c.notif_methods():
    last = [n for n in c.notifications if n.get("method") == "notifications/resources/updated"][-1]
    print("updated params:", json.dumps(last.get("params"))[:260])
c.close()

# config misuse: capture stderr synchronously via subprocess directly
p = subprocess.Popen([r"D:\codes\zorm-mcp\bin\zorm_mcp_server.exe", "--config",
                      ROOT + r"\zorm-mcp.json"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
except Exception:
    p.kill()
    out, err = p.communicate()
print("misuse exit:", p.poll())
print("misuse stdout:", out[:300])
print("misuse stderr:", err[:400])

p = subprocess.Popen([r"D:\codes\zorm-mcp\bin\zorm_mcp_server.exe", "--definitely-not-a-flag"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
except Exception:
    p.kill()
    out, err = p.communicate()
print("badflag exit:", p.poll())
print("badflag stdout:", out[:200])
print("badflag stderr:", err[:300])
