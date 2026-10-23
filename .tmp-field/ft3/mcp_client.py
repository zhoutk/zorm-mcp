"""Field-test harness for zorm-mcp: a raw MCP JSON-RPC stdio client.

Speaks newline-delimited JSON-RPC directly to bin/zorm_mcp_server.exe so we
can drive protocol-level behaviour that fixed GUI clients cannot reach:
MRTR requestState retries, legacy protocol versions, notifications, errors.
"""
import json
import os
import subprocess
import threading
import queue
import time

ROOT = r"D:\codes\zorm-mcp"
EXE = os.path.join(ROOT, "bin", "zorm_mcp_server.exe")
WORK = os.path.join(ROOT, ".tmp-field", "ft3")

DEFAULT_ENV = {
    "SystemRoot": os.environ.get("SystemRoot", r"C:\Windows"),
    "SystemDrive": os.environ.get("SystemDrive", "C:"),
    "TEMP": os.environ.get("TEMP", r"C:\Windows\Temp"),
    "TMP": os.environ.get("TMP", r"C:\Windows\Temp"),
    "USERNAME": os.environ.get("USERNAME", "user"),
    "COMSPEC": os.environ.get("ComSpec", r"C:\Windows\system32\cmd.exe"),
    "LOCALAPPDATA": os.environ.get("LOCALAPPDATA", ""),
    "NUMBER_OF_PROCESSORS": "4",
}


class Client:
    def __init__(self, args=(), cwd=WORK, clean_env=False, name="c"):
        env = dict(DEFAULT_ENV) if clean_env else os.environ.copy()
        kw = {}
        if hasattr(subprocess, "CREATE_NO_WINDOW"):
            kw["creationflags"] = subprocess.CREATE_NO_WINDOW
        self.p = subprocess.Popen(
            [EXE] + list(args), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, cwd=cwd, env=env, text=True,
            encoding="utf-8", errors="replace", bufsize=1, **kw)

        self.name = name
        self.notifications = []
        self.stderr_lines = []
        self._q = queue.Queue()
        self._id = 0
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._err_reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except Exception:
                self._q.put(("RAW", line))
                continue
            if "method" in msg and "id" not in msg:
                self._q.put(("NOTIF", msg))
            elif "id" in msg:
                self._q.put(("RESP", msg))

    def _err_reader(self):
        for line in self.p.stderr:
            self.stderr_lines.append(line.rstrip())

    def request(self, method, params=None, timeout=25):
        deadline = time.time() + timeout
        self._id += 1
        rid = self._id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"{self.name}: no reply to id={rid} ({method}) in {timeout}s")
            try:
                kind, m = self._q.get(timeout=min(left, 0.5))
            except queue.Empty:
                continue
            if kind == "RESP":
                if m.get("id") == rid:
                    return m
                continue  # late reply to some other request: ignore
            if kind == "NOTIF":
                self.notifications.append(m)
            elif kind == "RAW":
                self.notifications.append({"method": "_raw_stdout", "params": {"line": m}})

    def listen(self, params):
        """Send subscriptions/listen (2026): a request that never returns a
        JSON-RPC response - only the acknowledged notification first."""
        self._id += 1
        rid = self._id
        msg = {"jsonrpc": "2.0", "id": rid, "method": "subscriptions/listen", "params": params}
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        # wait for the ack notification
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                kind, m = self._q.get(timeout=0.5)
            except queue.Empty:
                continue
            if kind == "NOTIF":
                self.notifications.append(m)
                if m.get("method") == "notifications/subscriptions/acknowledged":
                    return m

    def call(self, name, arguments, params_extra=None, timeout=25):
        params = {"name": name, "arguments": arguments}
        if params_extra:
            params.update(params_extra)
        return self.request("tools/call", params, timeout=timeout)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg, separators=(",", ":")) + "\n")
        self.p.stdin.flush()

    def wait_notifs(self, seconds=1.0):
        end = time.time() + seconds
        while time.time() < end:
            try:
                kind, m = self._q.get(timeout=0.1)
            except queue.Empty:
                continue
            if kind == "NOTIF":
                self.notifications.append(m)

    def notif_methods(self):
        return [n.get("method") for n in self.notifications]

    def close(self, hard=False):
        try:
            if hard:
                self.p.kill()
            else:
                self.p.stdin.close()
                try:
                    self.p.wait(timeout=15)
                except Exception:
                    self.p.kill()
        except Exception:
            pass


def init(client, protocol="2026-07-28"):
    """initialize + notifications/initialized; returns the result object."""
    r = client.request("initialize", {
        "protocolVersion": protocol,
        "capabilities": {"roots": {"listChanged": False}},
        "clientInfo": {"name": "ft3", "version": "0.3"},
    }, timeout=20)
    client.notify("notifications/initialized")
    return r.get("result", r)


def tool_names(client):
    r = client.request("tools/list", {})
    return sorted(t["name"] for t in r["result"]["tools"])


def text_of(resp):
    """Extract concatenated text content of a tools/call result."""
    res = resp.get("result", {})
    return "".join(c.get("text", "") for c in res.get("content", []) if isinstance(c, dict))


def call_json(client, name, args, **kw):
    """Call a tool and parse its single JSON text payload."""
    r = client.call(name, args, **kw)
    res = r.get("result", {})
    if res.get("resultType") == "input_required":
        return {"__input_required__": True, "requestState": res.get("requestState", ""),
                "message": text_of(r), "raw": res}
    err = r.get("error")
    if err:
        return {"__error__": err.get("code"), "message": err.get("message")}
    try:
        return json.loads(text_of(r))
    except Exception:
        return {"__unparsed__": text_of(r), "raw": res}


def approve(client, name, args, state):
    """MRTR approved retry: requestState/inputResponses at the params level."""
    return client.call(name, args,
                       params_extra={"requestState": state,
                                     "inputResponses": {"confirm": {"approved": True}}},
                       timeout=30)


def decline(client, name, args, state):
    return client.call(name, args,
                       params_extra={"requestState": state,
                                     "inputResponses": {"confirm": {"approved": False}}},
                       timeout=30)


def write_config(cfg: dict):
    """Write a temp config file and return its absolute path."""
    p = os.path.join(WORK, "cfg-%d.json" % int(time.time() * 1000))
    with open(p, "w", encoding="utf-8") as f:
        json.dump(cfg, f)
    return p


def sqlite_cfg(fname):
    return {"databases": {"sqlite3": {"type": "sqlite3", "options": {
        "connString": os.path.join(WORK, fname), "DbLogClose": True,
        "parameterized": True}}}}
