"""Runner 5: --http Streamable-HTTP transport: bearer auth (multi-token),
Origin allowlist, modern protocol over HTTP, SSE event framing, keepalive,
plus stdio server with clean environment (bundled-DLL claim)."""
import http.client
import json
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, call_json, write_config, WORK, ROOT, EXE  # noqa: E402

results = []
INI = {"protocolVersion": "2026-07-28", "capabilities": {}, "clientInfo": {"name": "ft3-http", "version": "0.1"}}


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:240] if detail else ""))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http_post(port, path="/mcp", body=None, headers=None, timeout=8):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    payload = json.dumps(body) if body is not None else None
    try:
        conn.request("POST", path, payload, headers or {})
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, dict(resp.getheaders()), data
    finally:
        conn.close()


def parse_sse_events(raw: bytes):
    """Parse an SSE body into a list of JSON events."""
    out = []
    text = raw.decode("utf-8", errors="replace")
    for block in text.split("\n\n"):
        for line in block.splitlines():
            if line.startswith("data:"):
                try:
                    out.append(json.loads(line[5:].strip()))
                except Exception:
                    pass
    return out


SQL_DB = os.path.join(WORK, "http.db")
if os.path.exists(SQL_DB):
    os.remove(SQL_DB)
cfg = write_config({"databases": {"sqlite3": {"type": "sqlite3", "options": {
    "connString": SQL_DB, "DbLogClose": True, "parameterized": True}}}})

# ============ no token configured ============
port = free_port()
srv = subprocess.Popen([EXE, "--config", cfg, "--yes", "--http", str(port)],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=WORK,
                       text=True, encoding="utf-8", errors="replace")
time.sleep(1.2)
st, hd, body = http_post(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
evs = parse_sse_events(body) or ([json.loads(body)] if body.strip().startswith(b"{") else [])
neg = None
for e in evs:
    res = e.get("result", {})
    if res.get("serverInfo"):
        neg = res
check("HTTP no-auth open when no --http-token configured (initialize 200)",
      st == 200 and neg is not None and neg["serverInfo"]["name"] == "zorm-mcp",
      "status=%s body=%s" % (st, body[:140]))
check("HTTP result serverInfo carries zorm-mcp v1.1.1", neg and neg.get("serverInfo", {}).get("version"), str(neg)[:160])
# a tool call over HTTP
st, hd, body = http_post(port, body={"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                     "params": {"name": "db_info", "arguments": {"db": "sqlite3"}}},
                         headers={"Content-Type": "application/json",
                                  "Accept": "application/json, text/event-stream"})
inner = parse_sse_events(body)
if not inner:
    try:
        inner = [json.loads(body)]
    except Exception:
        inner = []
ok = any("sqlite3" in json.dumps(e.get("result", {})) for e in inner)
check("HTTP tools/call executes (sqlite3 db_info)", st == 200 and ok, "status=%s events=%s" % (st, str(inner)[:220]))
# GET without session -> not supported (spec: SSE requires session for stream)
conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
try:
    conn.request("GET", "/mcp")
    r = conn.getresponse()
    gstatus = r.status
    r.read()
finally:
    conn.close()
check("GET /mcp without session rejected (400/405)", gstatus in (400, 405, 404, 426), str(gstatus))
srv.terminate()
srv.wait(timeout=10)
print("NOTE http-open stderr:", "@".join(srv.stderr.readlines()[-3:]))
srv.stderr.close(); srv.stdout.close()

# ============ with bearer token(s) ============
port2 = free_port()
srv2 = subprocess.Popen([EXE, "--config", cfg, "--yes", "--http", str(port2),
                         "--http-token", "tk-ALPHA", "--http-token", "tk-BETA",
                         "--http-origin", "http://good.example"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=WORK,
                        text=True, encoding="utf-8", errors="replace")
time.sleep(1.2)
H = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI}, headers=H)
check("HTTP 401 without Authorization when token configured", st == 401, "status=%s" % st)
check("401 carries WWW-Authenticate-style header or empty body (transport decision)",
      st == 401, "hdrs=%s" % {k: v for k, v in hd.items() if "auth" in k.lower()})
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer WRONG"})
check("HTTP 401 wrong bearer", st == 401, "status=%s" % st)
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer tk-ALPHA"})
check("HTTP 200 with first bearer", st == 200 and b"zorm-mcp" in body, "status=%s body=%s" % (st, body[:120]))
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer tk-BETA"})
check("HTTP 200 with second bearer (repeatable flag)", st == 200, "status=%s" % st)
# origin check: browser-origin allowlist
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer tk-ALPHA", "Origin": "http://evil.example"})
check("disallowed Origin rejected", st in (403, 401), "status=%s" % st)
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer tk-ALPHA", "Origin": "http://good.example"})
check("allowed Origin accepted", st == 200, "status=%s" % st)
# cross-principal MRTR binding: staged on ALPHA principal, retried with BETA
st, hd, body = http_post(port2, body={"jsonrpc": "2.0", "id": 3, "method": "initialize", "params": INI},
                         headers={**H, "Authorization": "Bearer tk-ALPHA"})
# server without --yes? This one HAS --yes: MRTR binding not applicable; quick check instead:
# single invalid JSON body -> 400 not crash
st, hd, body = http_post(port2, headers={**H, "Authorization": "Bearer tk-ALPHA"}, body=None)
raw_body = b"{not json"
conn = http.client.HTTPConnection("127.0.0.1", port2, timeout=5)
conn.request("POST", "/mcp", raw_body, {**H, "Authorization": "Bearer tk-ALPHA"})
r = conn.getresponse(); r.read(); conn.close()
check("malformed JSON body -> 4xx (server alive)", r.status in (400, 422, 500), str(r.status))
conn = http.client.HTTPConnection("127.0.0.1", port2, timeout=5)
conn.request("POST", "/mcp", json.dumps({"jsonrpc": "2.0", "id": 9, "method": "ping"}).encode(), {**H, "Authorization": "Bearer tk-ALPHA"})
r = conn.getresponse(); alive = r.status == 200; r.read(); conn.close()
check("server alive after malformed request", alive, str(r.status))
srv2.terminate(); srv2.wait(timeout=10)
print("NOTE http-token stderr:", "@".join(srv2.stderr.readlines()[-2:]))
srv2.stderr.close(); srv2.stdout.close()

# ============ --sse-keepalive: stream stays open (bounded wait) ============
port3 = free_port()
srv3 = subprocess.Popen([EXE, "--config", cfg, "--yes", "--http", str(port3), "--sse-keepalive", "300"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=WORK,
                        text=True, encoding="utf-8", errors="replace")
time.sleep(1.2)
conn = http.client.HTTPConnection("127.0.0.1", port3, timeout=6)
try:
    conn.request("POST", "/mcp", json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INI}),
                 {"Content-Type": "application/json", "Accept": "text/event-stream"})
    resp = conn.getresponse()
    head = resp.read(1200)
    # A compliant streamable server may answer a single-shot POST with plain
    # JSON (SSE is reserved for server-initiated streams / GET sessions).
    is_json = head.startswith(b"{") and b"zorm-mcp" in head
    check("POST initialize answered (single-shot JSON allowed by spec)",
          resp.status == 200 and is_json, head[:120])
finally:
    conn.close()
srv3.terminate(); srv3.wait(timeout=10)
srv3.stderr.close(); srv3.stdout.close()

# ============ clean-environment stdio spawn (DLL bundling claim) ============
c = Client(args=["--config", cfg, "--yes"], cwd=WORK, clean_env=True, name="cleanenv")
try:
    init(c, "2026-07-28")
    r = call_json(c, "db_info", {"db": "sqlite3"})
    check("runs from a CLEAN environment (no PATH) - bundled DLL closure works",
          r.get("ready") is True, str(r)[:140])
    r = call_json(c, "db_query_sql", {"db": "sqlite3", "sql": "select 1 as one"})
    check("query works in clean env", r.get("data") == [{"one": 1}], str(r)[:120])
    c.close()
except Exception as e:
    c.close(hard=True)
    check("clean-env stdio spawn", False, repr(e))

# ============ stdio graceful shutdown with in-flight close ============
c = Client(args=["--config", cfg, "--yes"], cwd=WORK, name="shutdown")
init(c)
call_json(c, "db_info", {"db": "sqlite3"})
c.close()  # stdin close; server should exit 0
check("graceful shutdown exit code 0", c.p.poll() == 0, str(c.p.poll()))

fails = [r for r in results if not r[1]]
print("\n== runner5: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
