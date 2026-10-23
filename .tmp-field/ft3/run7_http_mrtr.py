"""Runner 7: HTTP MRTR principal binding + misc CLI validation."""
import http.client
import json
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, WORK, EXE  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:240] if detail else ""))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http_post(port, body, headers=None, timeout=8):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("POST", "/mcp", json.dumps(body) if body is not None else None,
                     headers or {"Content-Type": "application/json",
                                 "Accept": "application/json, text/event-stream"})
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, data, dict(resp.getheaders())
    finally:
        conn.close()


def parse(body: bytes):
    text = body.decode("utf-8", errors="replace")
    if text.startswith("{"):
        return [json.loads(text)]
    out = []
    for block in text.split("\n\n"):
        for line in block.splitlines():
            if line.startswith("data:"):
                try:
                    out.append(json.loads(line[5:].strip()))
                except Exception:
                    pass
    return out


SQL_DB = os.path.join(WORK, "http2.db")
for f in (SQL_DB, SQL_DB + "-wal", SQL_DB + "-shm"):
    if os.path.exists(f):
        os.remove(f)
cfg = os.path.join(WORK, "cfg-http2.json")
with open(cfg, "w") as f:
    json.dump({"databases": {"sqlite3": {"type": "sqlite3", "options": {
        "connString": SQL_DB, "DbLogClose": True, "parameterized": True}}}}, f)

# bootstrap table via stdio --yes
boot = Client(args=["--config", cfg, "--yes"], name="boot7")
init(boot)
call_json(boot, "db_trans", {"db": "sqlite3", "sqls": [{"text": "create table t (id text primary key, v text)"}]})
boot.close()

# HTTP server WITHOUT --yes: MRTR active, bound to bearer principal
port = free_port()
srv = subprocess.Popen([EXE, "--config", cfg, "--http", str(port),
                        "--http-token", "tk-A", "--http-token", "tk-B"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=WORK,
                       text=True, encoding="utf-8", errors="replace")
time.sleep(1.2)
H = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}


def call_http(token, name, args, extra=None, rid=1):
    body = {"jsonrpc": "2.0", "id": rid, "method": "tools/call",
            "params": {"name": name, "arguments": args}}
    if extra:
        body["params"].update(extra)
    st, data, _ = http_post(port, body, {**H, "Authorization": "Bearer " + token})
    evs = parse(data) or [{}]
    return st, evs[0]


st, r = call_http("tk-A", "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "v": "x"}})
res = r.get("result", {})
check("HTTP stage via principal A", st == 200 and res.get("resultType") == "input_required", str(r)[:200])
state = res.get("requestState", "")

# cross-principal: token B replays A's requestState -> must be rejected
st, r = call_http("tk-B", "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "v": "x"}},
                  extra={"requestState": state, "inputResponses": {"confirm": {"approved": True}}})
err = r.get("error", {})
check("cross-principal requestState rejected over HTTP",
      err.get("code") == -32602 and "different tool" in err.get("message", ""), str(r)[:240])

# same principal retry executes
st, r = call_http("tk-A", "db_insert", {"db": "sqlite3", "table": "t", "data": {"id": "1", "v": "x"}},
                  extra={"requestState": state, "inputResponses": {"confirm": {"approved": True}}}, rid=5)
res = r.get("result", {})
check("same-principal approved retry executes over HTTP",
      st == 200 and res.get("resultType") != "input_required" and "affected" in json.dumps(res), str(r)[:200])

# instructions available over HTTP initialize
st, data, _ = http_post(port, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                               "params": {"protocolVersion": "2026-07-28", "capabilities": {},
                                          "clientInfo": {"name": "ft7", "version": "0"}}},
                        {**H, "Authorization": "Bearer tk-A"})
evs = parse(data)
ok = any(len(e.get("result", {}).get("instructions", "")) > 200 for e in evs)
check("initialize over HTTP carries instructions", ok, str(evs)[:160])

srv.terminate()
srv.wait(timeout=10)
srv.stderr.close(); srv.stdout.close()

# ---- --sse-keepalive bogus: FIXED in dcefe71 -> strict parse, exit 2 ----
kk = free_port()
p = subprocess.Popen([EXE, "--config", cfg, "--http", str(kk), "--sse-keepalive", "bogus"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=WORK, text=True, encoding="utf-8", errors="replace")
try:
    out, err = p.communicate(timeout=10)
    serving = False
except Exception:
    p.kill()
    out, err = p.communicate()
    serving = "HTTP serving" in (err or "")
check("F-6 FIXED --sse-keepalive 'bogus' -> exit 2 with invalid message",
      p.poll() == 2 and "invalid --sse-keepalive" in (err or ""), "exit=%s stderr=%s" % (p.poll(), (err or "")[-160:].replace("\n", " | ")))

# ---- bearer in stdio mode ignored (http flags are http-only) ----
c2 = Client(args=["--config", cfg], cwd=WORK, name="stdio-nogui")
init(c2, "2024-11-05")
try:
    r = c2.request("tools/call", {"name": "db_info", "arguments": {"db": "sqlite3"}})
    check("stdio serves without bearer setup (legacy 2024 init + call)", "result" in r, str(r)[:120])
except Exception as e:
    check("stdio legacy serve", False, repr(e))
c2.close()

fails = [r for r in results if not r[1]]
print("\n== runner7: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
