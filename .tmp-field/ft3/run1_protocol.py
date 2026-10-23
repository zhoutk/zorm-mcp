"""Runner 1: protocol surface - initialize (5 revisions), tools/resources/
prompts, legacy vs modern era, notifications, graceful shutdown."""
import json
import sys

sys.path.insert(0, r"D:\codes\zorm-mcp\.tmp-field\ft3")
from mcp_client import Client, init, tool_names, ROOT  # noqa: E402

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + name + ("  | " + str(detail)[:220] if detail else ""))


c = Client(args=["--config", ROOT + r"\zorm-mcp.db.tmpcfg.json"], name="proto")
# No config at all: built-in defaults (sqlite3 + jsonfile) - this is the real
# registered path. Use no args instead.
c.close(hard=True)
c = Client(args=[], name="proto")

# ---- initialize negotiation over every advertised revision ----
EXPECTED_TOOLS_12 = ["db_count", "db_delete", "db_describe_table", "db_indexes",
                     "db_info", "db_insert", "db_insert_batch", "db_list_tables",
                     "db_query", "db_query_sql", "db_trans", "db_update"]  # no db_exec_sql without --allow-sql
revs = ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"]
for rev in revs:
    try:
        r = init(c, protocol=rev)
        check("initialize %s" % rev,
              r.get("protocolVersion") == rev and r.get("serverInfo", {}).get("name") == "zorm-mcp",
              "negotiated=%s name=%s keys=%s" % (r.get("protocolVersion"),
                                                 r.get("serverInfo", {}).get("name"),
                                                 sorted(r.keys())))
        tn = tool_names(c)
        check("tools/list after %s = 12 tools" % rev, tn == EXPECTED_TOOLS_12, str(tn))
        instr = r.get("instructions") or ""
        check("instructions present (%s)" % rev, len(instr) > 200 and "db_query" in instr,
              "len=%d" % len(instr))
    except Exception as e:
        check("initialize %s" % rev, False, repr(e))

# ---- capabilities advertised ----
try:
    r = init(c, protocol="2026-07-28")
    caps = r.get("capabilities", {})
    check("capabilities.tools present", "tools" in caps, str(caps))
    check("capabilities.resources + subscribe", caps.get("resources", {}).get("subscribe") is True
          and caps.get("resources", {}).get("listChanged") is True, str(caps.get("resources")))
    check("capabilities.prompts + completions", "prompts" in caps and "completions" in caps, str(caps))
    print("NOTE capabilities (informational):", json.dumps(caps))
except Exception as e:
    check("capabilities", False, repr(e))

# ---- ping ----
try:
    r = c.request("ping", {})
    check("ping", "result" in r, str(r)[:120])
except Exception as e:
    check("ping", False, repr(e))

# ---- resources: application/json content with URI templates ----
try:
    r = c.request("resources/list", {})
    r2 = c.request("resources/templates/list", {})
    uris = [x["uri"] for x in r["result"]["resources"]]
    tpluris = [x.get("uriTemplate") for x in
               (r["result"].get("templates") or r["result"].get("resourceTemplates")
                or r2["result"].get("templates") or r2["result"].get("resourceTemplates") or [])]
    check("db://guide + db://schema fixed resources", "db://guide" in uris and "db://schema" in uris, str(uris))
    check("2 URI templates (per-db, per-table)", sorted(tpluris) ==
          ["db://schema/{db}", "db://schema/{db}/{table}"], str(tpluris))

    g = c.request("resources/read", {"uri": "db://schema"})
    txt = "".join(x.get("text", "") for x in g["result"]["contents"])
    check("db://schema lists both backends", "sqlite3" in txt and "jsonfile" in txt, txt[:200])

    g = c.request("resources/read", {"uri": "db://guide"})
    txt = "".join(x.get("text", "") for x in g["result"]["contents"])
    check("db://guide is the playbook", len(txt) > 1000 and "MRTR" in txt, "len=%d" % len(txt))

    g = c.request("resources/read", {"uri": "db://schema/sqlite3"})
    contents = g["result"]["contents"]
    txt = "".join(x.get("text", "") for x in contents)
    check("db://schema/sqlite3 has empty-table hint or table list",
          "t_e2e" in txt or "hint" in txt.lower() or txt == "", repr(txt[:120]))

    g = c.request("resources/read", {"uri": "db://schema/nosuchdb"})
    check("template uri for unknown db -> teaching error",
          g.get("error", {}).get("code") == -32602 and "available" in g["error"]["message"],
          str(g.get("error"))[:200])
    g = c.request("resources/read", {"uri": "db://nope/x"})
    check("unmatched uri -> Unknown resource", g.get("error", {}).get("message", "").startswith("Unknown resource"),
          str(g.get("error"))[:120])
except Exception as e:
    check("resources", False, repr(e))

# ---- prompts + completion ----
try:
    r = c.request("prompts/list", {})
    names = [p["name"] for p in r["result"]["prompts"]]
    check("prompts/list has db-playbook", "db-playbook" in names, str(names))
    r = c.request("prompts/get", {"name": "db-playbook", "arguments": {}})
    txt = "".join(m.get("content", {}).get("text", "") for m in r["result"].get("messages", []))
    check("db-playbook prompt content", len(txt) > 500 and "db_query" in txt, "len=%d" % len(txt))
    r = c.request("completion/complete", {"ref": {"type": "ref/resource", "uri": "db://schema/{db}"},
                                          "argument": {"name": "db", "value": "sql"}})
    compl = r["result"].get("completion") or r["result"]  # mcp-cpp flattens values at result level
    vals = compl.get("values", []) if isinstance(compl, dict) else []
    check("completion values for db='sql' -> sqlite3", "sqlite3" in vals,
          "result=%s" % json.dumps(r.get("result", {}))[:120])
except Exception as e:
    check("prompts/completion", False, repr(e))

# ---- modern era: per-request `_meta` behaviour on tools/call ----
try:
    r = c.request("tools/call", {"name": "db_info", "arguments": {"db": "sqlite3"},
                                 "_meta": {"progressToken": 9}})
    res = r.get("result", {})
    # 2026 spec: server may attach _meta.serverInfo to results; progressToken
    # echo is optional (only required for progress notifications).
    check("tools/call with _meta succeeds", "result" in r and "content" in res,
          "resultType=%s" % res.get("resultType"))
    print("NOTE tools/call _meta on result:", json.dumps(res.get("_meta")))
except Exception as e:
    check("tools/call _meta", False, repr(e))

# ---- unknown method is a clean -32601, stderr stays silent ----
try:
    before = len(c.stderr_lines)
    r = c.request("bogus/method", {})
    check("unknown method -> -32601", r.get("error", {}).get("code") == -32601, str(r.get("error"))[:150])
    check("stderr silent on unknown method (stdio purity)", len(c.stderr_lines) == before,
          str(c.stderr_lines[-3:]))
except Exception as e:
    check("unknown method", False, repr(e))

# ---- malformed JSON on stdin must not kill the process ----
try:
    c.p.stdin.write("this is not json\n")
    c.p.stdin.flush()
    r = c.request("ping", {}, timeout=15)
    check("server survives garbage line on stdin", "result" in r, str(r)[:100])
except Exception as e:
    check("garbage stdin", False, repr(e))

# ---- stdio purity across the whole session ----
try:
    check("0 non-JSON lines on stdout for all sessions above",
          not any(n.get("method") == "_raw_stdout" for n in c.notifications),
          c.notifications[:2])
except Exception as e:
    check("stdio purity", False, repr(e))

# ---- graceful shutdown: stdin EOF -> clean exit, no stderr noise ----
c.close()
code = c.p.poll()
check("clean exit at stdin EOF", code == 0, "exit=%s stderr=%s" % (code, c.stderr_lines[-5:]))

fails = [r for r in results if not r[1]]
print("\n== runner1: %d checks, %d failed ==" % (len(results), len(fails)))
for n, _, d in fails:
    print("FAILED:", n, "|", d)
