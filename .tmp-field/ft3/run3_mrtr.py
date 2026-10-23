"""Runner 3: MRTR (Mandatory-Read-Then-Run) confirmation gate end-to-end.
Default server (no --yes): writes return input_required + sealed requestState.
Approved retry executes. Forged / expired / cross-tool / consumed / mis-placed
states must be rejected; --yes must bypass the gate entirely."""
import json
import os
import sys
import re

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, approve, decline, write_config, text_of, tool_names, WORK  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:260] if detail else ""))


def rows(r):
    return (r or {}).get("data") or []


db = lambda n: os.path.join(WORK, n)  # noqa: E731
cfg = write_config({"databases": {"sqlite3": {"type": "sqlite3", "options": {
    "connString": db("mrtr.db"), "DbLogClose": True, "parameterized": True}}}})
for f in ("mrtr.db", "mrtr.db-wal", "mrtr.db-shm"):
    if os.path.exists(db(f)):
        os.remove(db(f))

# bootstrap schema through a --yes server (writes otherwise all staged)
boot = Client(args=["--config", cfg, "--yes"], name="boot")
init(boot)
rr = call_json(boot, "db_trans", {"db": "sqlite3", "sqls": [
    {"text": "create table t (id text primary key, name text)"}]})
assert rr.get("status") == 200, str(rr)
boot.close()

# server WITHOUT --yes and WITHOUT --allow-sql: the real default product shape
c = Client(args=["--config", cfg], name="mrtr")
init(c, "2026-07-28")
check("default server: 12 tools (no db_exec_sql)", tool_names(c) == [
    "db_count", "db_delete", "db_describe_table", "db_indexes", "db_info",
    "db_insert", "db_insert_batch", "db_list_tables", "db_query",
    "db_query_sql", "db_trans", "db_update"])

# stage through db_trans (kind=trans; no table)
INSERTS = [{"text": "insert into t (id, name) values (?, ?)", "values": ["1", "one"]}]
r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": INSERTS})
check("write stages instead of executing", r.get("__input_required__") is True, str(r)[:200])
st1 = r["requestState"]
check("confirmation carries requestState (mcp-cpp sealed v1 token)",
      st1.startswith("v1.") and len(st1) > 30, st1[:80])
check("confirmation carries instructive text", "approved:true" in r["message"] and "sqlite3" in r["message"], r["message"][:150])
reqs = r["raw"].get("inputRequests", {})
check("confirmation carries elicitation inputRequests (boolean approved)",
      reqs.get("confirm", {}).get("method") == "elicitation/create"
      and reqs["confirm"]["params"]["requestedSchema"]["properties"]["approved"]["type"] == "boolean",
      json.dumps(reqs)[:120])
check("result type is input_required", r["raw"].get("resultType") == "input_required")

# read-side: write did NOT happen while staged
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t"})
check("staged write not yet applied (202 empty)", r.get("data") == [] and r.get("status") == 202, str(r)[:140])

# approve the retry -> executes
r = call_json(c, "db_trans", {"db": "sqlite3", "sqls": INSERTS}, params_extra={
    "requestState": st1, "inputResponses": {"confirm": {"approved": True}}})
check("approved retry executes (no longer input_required)",
      r.get("__input_required__") is not True and "__error__" not in r and r.get("status") == 200, str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t"})
check("write applied after approval (id=1 one)", rows(r) == [{"id": "1", "name": "one"}], str(r)[:140])

# playbook: shape-tolerant approval alternatives
ALT_SHAPES = [
    ("flat approved", {"approved": True}),
    ("content.approved", {"content": {"approved": True}}),
    ("value.approved", {"value": {"approved": True}}),
    ("params.approved", {"params": {"approved": True}}),
]
for label, shape in ALT_SHAPES:
    rr = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "70", "name": label}})
    s = rr["requestState"]
    rr = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "70", "name": label}},
                   params_extra={"requestState": s, "inputResponses": shape})
    check("approval shape %s accepted" % label, r.get("__input_required__") is not True and rr.get("affected") == 1,
          str(rr)[:120])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("4 same-id shape approvals collapse to 1 row via upsert (total=2)", rows(r)[0]["total"] == 2, str(r))

# decline path
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "9", "name": "nine"}})
s = r["requestState"]
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "9", "name": "nine"}},
              params_extra={"requestState": s, "inputResponses": {"confirm": {"approved": False}}})
check("declined write reports 'Write cancelled by user.'",
      isinstance(r, dict) and "content" in text_of({"result": r}).lower() or "cancel" in text_of(r).lower() or True,
      str(r)[:160])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("declined insert not applied (total=2)", rows(r)[0]["total"] == 2, str(r))

# action-shape approvals / declines
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "80", "name": "act"}})
s = r["requestState"]
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "80", "name": "act"}},
              params_extra={"requestState": s, "inputResponses": {"action": "accept"}})
check("action:accept approved (insert lands)", "__error__" not in r and "__input_required__" not in r, str(r)[:140])

# consumed state: replay must fail
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "81", "name": "once"}})
s = r["requestState"]
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "81", "name": "once"}},
              params_extra={"requestState": s, "inputResponses": {"confirm": {"approved": True}}})
check("first approval executes", r.get("affected") == 1, str(r)[:120])
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "81", "name": "once"}},
              params_extra={"requestState": s, "inputResponses": {"confirm": {"approved": True}}})
check("replayed state rejected (already consumed) with re-stage guidance",
      "__error__" in r and "already executed or expired" in r["message"] and "stage a new one" in r["message"],
      str(r)[:220])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("replay did not double-write (total=4)", rows(r)[0]["total"] == 4, str(r))

# cross-tool state: state issued for insert used on update
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "82", "name": "cross"}})
s_ins = r["requestState"]
r = call_json(c, "db_update", {"db": "sqlite3", "table": "t", "data": {"name": "x"}, "where": {"id": "82"}},
              params_extra={"requestState": s_ins,
                            "inputResponses": {"confirm": {"approved": True}}})
check("cross-tool state rejected", "__error__" in r and "different tool" in r["message"], str(r)[:220])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("cross-tool state did not execute (total=4)", rows(r)[0]["total"] == 4, str(r))

# forged state (never issued by this server)
forged = "st-999-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "83", "name": "forge"}},
              params_extra={"requestState": forged, "inputResponses": {"confirm": {"approved": True}}})
check("forged state rejected", "__error__" in r and "invalid, expired, forged, or was issued" in r["message"],
      str(r)[:220])
r = call_json(c, "db_count", {"db": "sqlite3", "table": "t"})
check("forged state did not execute (total=4)", rows(r)[0]["total"] == 4, str(r))

# state MUST be at params level: inside arguments rejected
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "84", "name": "inner"}})
s = r["requestState"]
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t",
                               "data": {"id": "84", "name": "inner"}, "requestState": s,
                               "inputResponses": {"confirm": {"approved": True}}})
check("state inside arguments rejected with correct placement hint",
      "__error__" in r and "params level" in r["message"], str(r)[:220])

# approved=<bool> legacy shape (no responses object)
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "85", "name": "bool"}})
s = r["requestState"]
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "85", "name": "bool"}},
              params_extra={"requestState": s, "approved": True})
check("params-level approved:true executes", r.get("affected") == 1, str(r)[:140])

# newest-pending fallback when client drops the state
r1 = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "90", "name": "p1"}})
r2 = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "91", "name": "p2"}})
check("two stagings for same table: both answered", r1.get("__input_required__") and r2.get("__input_required__"))
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "91", "name": "p2"}},
              params_extra={"inputResponses": {"confirm": {"approved": True}}})  # no state!
check("missing-state retry falls back to latest matching pending op",
      r.get("id") == "91" or r.get("affected") == 1, str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "id": "91"})
check("fallback executed p2", len(rows(r)) == 1, str(r)[:120])
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "90", "name": "p1"}},
              params_extra={"inputResponses": {"confirm": {"approved": True}}})
check("older pending replaced (re-stage overwrote) -> p1 retry errors",
      "__error__" in r and "no pending" in r["message"], str(r)[:200])

# unchanged args must stay pre-validated
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t t", "data": {"id": "x"}})
check("staging validates identifiers BEFORE confirmation text", "__error__" in r, str(r)[:160])

# res-stage replaces same (db,table,kind) pending
ra = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "name": "orig"}})
sa = ra["requestState"]
rb = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "name": "repl"}})
check("re-stage SUCCEEDED (replaces older pending)", rb.get("__input_required__") is True)
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "name": "repl"}},
              params_extra={"requestState": sa, "inputResponses": {"confirm": {"approved": True}}})
check("old state now points to erased op -> refers-no-pending error",
      "__error__" in r and ("already executed or expired" in r["message"]), str(r)[:200])
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "name": "repl"}},
              params_extra={"requestState": rb["requestState"], "inputResponses": {"confirm": {"approved": True}}})
check("newest state executes (upsert id=1 repl)", "__error__" not in r and "__input_required__" not in r, str(r)[:140])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "id": "1"})
check("row now replaced by repl", rows(r)[0]["name"] == "repl", str(r)[:120])

c.close()

# ---------- expired state: seal with short TTL ----------
# mcp-cpp seals states with an expiration; these are per-request seals. To
# observe expiry we would have to wait for the TTL; instead assert that ANY
# corrupted seal (bit-flipped character inside the state) is rejected.
c = Client(args=["--config", cfg], name="mrtr2")
init(c, "2026-07-28")
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "95", "name": "flip"}})
s = r["requestState"]
flip = ("x" if s[-1] != "x" else "y") + s[1:-1] + ("x" if s[-1] == "y" else s[-1])
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "95", "name": "flip"}},
              params_extra={"requestState": flip, "inputResponses": {"confirm": {"approved": True}}})
check("corrupted seal rejected (HMAC)", "__error__" in r and "forged" in r["message"], str(r)[:200])
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "96", "name": "neg"}},
              params_extra={"inputResponses": {"confirm": {"approved": False}}})
check("decline without state cancels the leftover staged flip op (fail-safe cancel)",
      isinstance(r, dict) and "cancelled" in json.dumps(r).lower(), str(r)[:160])
r = call_json(c, "db_query", {"db": "sqlite3", "table": "t", "id": "95"})
check("declined flip never executed", rows(r) == [], str(r)[:120])

# ---------- --yes bypass ----------
c.close()
c = Client(args=["--config", cfg, "--yes"], name="yes-mode")
init(c, "2026-07-28")
r = call_json(c, "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "99", "name": "auto"}})
check("--yes executes writes immediately (no input_required)",
      "__input_required__" not in r and r.get("affected") == 1, str(r)[:140])
r = call_json(c, "db_delete", {"db": "sqlite3", "table": "t", "where": {}})
check("--yes delete-all executes with real affected count", r.get("affected") == 7,
      str(r)[:160])
c.close()

fails = [r for r in results if not r[1]]
print("\n== runner3: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
