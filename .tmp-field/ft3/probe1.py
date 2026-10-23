"""Probe: resource template reads, completion shape, _meta echo behaviour."""
import json
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init  # noqa: E402

c = Client(args=[], name="probe")

for rev in ("2026-07-28",):
    r = init(c, protocol=rev)
    print("== init", rev, "caps:", json.dumps(r.get("capabilities")))

for m, p in [
    ("resources/templates/list", {}),
    ("resources/read", {"uri": "db://schema/sqlite3"}),
    ("resources/read", {"uri": "db://schema/sqlite3/t_e2e"}),
    ("resources/read", {"uri": "db://schema/jsonfile/missing_table"}),
    ("resources/read", {"uri": "db://schema/nosuchdb"}),
    ("resources/read", {"uri": "db://nope/x"}),
]:
    try:
        r = c.request(m, p, timeout=10)
        print(m, p.get("uri", ""), "=>", json.dumps(r)[:500])
    except Exception as e:
        print(m, p.get("uri", ""), "EXC", repr(e))

r = c.request("completion/complete", {"ref": {"type": "ref/resource", "uri": "db://schema/{db}"},
                                      "argument": {"name": "db", "value": "sql"}})
print("completion raw:", json.dumps(r))

r = c.request("tools/list", {"_meta": {"progressToken": 7}})
print("tools/list _meta on result:", json.dumps(r["result"].get("_meta")))
r = c.call("db_info", {"db": "sqlite3"})
print("tools/call with _meta? Try plain first:", json.dumps(r.get("error") or "ok")[:120])
r = c.request("tools/call", {"name": "db_info", "arguments": {"db": "sqlite3"},
                             "_meta": {"progressToken": 9}})
print("tools/call _meta echo:", json.dumps(r.get("result", {}).get("_meta"))[:200])
r = c.request("tools/call", {"name": "db_bogus", "arguments": {}})
print("tool error _meta:", json.dumps(r)[:300])

(init2 := None)
# legacy-era client: no _meta anywhere, older protocol version, then check a call
c2 = Client(args=[], name="legacy")
r = init(c2, protocol="2024-11-05")
r = c2.request("tools/call", {"name": "db_info", "arguments": {"db": "sqlite3"}})
print("legacy db_info:", json.dumps(r.get("result", r.get("error")))[:260])
c2.close()
c.close()
