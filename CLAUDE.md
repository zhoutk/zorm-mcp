# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`zorm-mcp` is a production MCP server exposing **13 structured database tools** over five backends
(sqlite3 / jsonfile / mysql / postgres / dm8) using one dialect-free tool surface. It links two
third-party projects kept as git submodules:

- `thirds/zorm` — the `zormlib` ORM (`ZORM::Idb`, `ZORM::DbBase`) + backend DLLs + zjson
- `thirds/mcp-cpp` — the C++ MCP protocol library

`demo/` is the earlier single-purpose validation showcase ("Zorm Analytics") that this product was
extracted from; it still builds and its e2e test runs, but new work belongs in `src/`. It builds two
servers: `zorm_analytics_server` (the multi-backend showcase) and `zorm_server` (a ~200-line
single-backend **template** for "database as MCP server" — no `db` routing param, no MRTR gate, 4
tools; its 2nd CLI arg is a JSON options object for `DbBase`, not a file path; it self-reports as
`zorm-mini-mcp` to avoid colliding with the product's `zorm-mcp`).

For zorm internals (backend/dialect architecture, `run-test`, the contract test suite), read
`thirds/zorm/CLAUDE.md` — that submodule is a full independent repo.

## Build & test (MSYS2 clang64 / Windows, Ninja)

```bash
git submodule update --init --recursive   # first checkout only
cmake --preset clang-rlz                  # Release (default); clang-dbg for Debug
cmake --build build -j 8
ctest --test-dir build                    # both e2e suites
```

- `build/` holds intermediate artifacts only; executables land in `bin/` (`zorm_mcp_server.exe`,
  the demo servers, test exes) — `RUNTIME_OUTPUT_DIRECTORY` is set in CMakeLists.
- Single e2e suite: `ctest --test-dir build -R mcp_test_zorm_mcp -V`
- Single gtest case: `bin/mcp_test_zorm_mcp.exe --gtest_filter=ZormMcpE2E.MrtrWriteConfirmation`
- The e2e test spawns `zorm_mcp_server.exe --child` as a real child over stdio pipes; it sets the
  child's `PATH` from `ZORM_DLL_DIR` (`thirds/zorm/thirds/bin/win10`) itself because ctest `ENVIRONMENT`
  escaping for `PATH` proved unreliable on Windows.
- zorm's own regression gate (only relevant when touching `thirds/zorm`): `cd thirds/zorm && ./run-test all`.

### Runtime DLL bundling (do not "fix" this by adding PATH)

`zorm_mcp_bundle_runtime_deps` (POST_BUILD) resolves the exe's **full dependency closure** and copies
it next to the executable — backend DLLs (`sqlite3.dll`/`libpq.dll`/`dmdpi.dll`) *and* `libc++.dll`,
`libmariadb.dll` and their transitive deps from `D:/msys64/clang64/bin`. AI clients spawn the server
with their own environment (no MSYS2 PATH), so this is the only reason "register and it just works".
A missing DLL shows up client-side as `Process exited with code 3221225781` (0xC0000135
`STATUS_DLL_NOT_FOUND`); rebuild the target rather than putting MSYS2 on the client's PATH.

## Working with the submodules

`thirds/zorm` and `thirds/mcp-cpp` are **independent repositories** (their own remotes, tracked on
`master` via `.gitmodules`). To change one, edit/commit/push inside the submodule, then record the new
revision here:

```bash
cd thirds/zorm && git add -A && git commit -m "..." && git push
cd ../.. && ./sync-thirds.sh      # = git submodule update --remote --merge + auto-commit the pointer
```

Committing inside the submodule alone leaves this repo pointing at the old revision (a later
`git submodule update` would revert it) — `sync-thirds.sh` exists to fix exactly that. mcp-cpp is
protocol-faithful: do not modify it for application needs.

## Source layout (`src/`, ~1.8k lines)

| File | Role |
| --- | --- |
| `main.cpp` | Arg parsing, `loadSpecs`/`openBackends`/`discoverAllSchemas`, `buildServer()` (registers every tool/resource/prompt), stdio vs `--http` serving |
| `app.hpp/.cpp` | Global `App` state (backend registry + `specs`, schema cache, MRTR pending table, `--yes`/`--allow-sql` flags, `weak_ptr<Server>` for write notifications), startup (`loadSpecs`/`openBackends`/`discoverAllSchemas`) and the dialect-routed catalogs behind `listTables`/`describeTable`. The three `type ==` chains are `sqlListTables` / `sqlPrimaryKeys` / `sqlDescribe`; none of this belongs in a tool handler |
| `guide.hpp/.cpp` | The AI-comprehension layer: `instructionsText()` decision table, `playbookText()`, `db-playbook` prompt |
| `json_util.hpp/.cpp` | zjson↔mcp-cpp bridging, `okStatus`, result envelopes, `rowVal`/`rowStr`, `validIdentifier` |
| `meta_tools.cpp` | `db_list_tables` / `db_describe_table` / `db_indexes` / `db_info` |
| `data_tools.cpp` | `db_query` / `db_count` + all CRUD writes; owns the shared MRTR machinery |
| `sql_tools.cpp` | `db_query_sql` (read-only gate) / `db_trans` / `db_exec_sql` |
| `resources.cpp` | `db://schema` (+ `{db}` / `{db}/{table}` URI **templates**) and `db://guide`, completion |
| `tools.hpp` | Registration entry points + `stageWrite`/`executeWrite`/`inputRequiredConfirm` decls |

Layout rules (why it is flat, and which files get a header):

- A `.hpp` exists only where declarations are shared by **multiple TUs** (`app`, `json_util`,
  `guide`, `tools`) — the header count equals real coupling, not module count. `tools.hpp` has no
  `.cpp`: it is the wiring panel between `main` and the three tool TUs. `meta_tools.cpp` /
  `data_tools.cpp` / `sql_tools.cpp` / `resources.cpp` need no header because `main` calls each
  exactly once.
- **No project header includes another project header.** Cross-module use happens only inside
  `.cpp` files, so the layering (`main` → `tools`/`guide`/`app` → `json_util` → third-party) is
  enforced by include direction — which is why the flat directory needs no subdirectories.
- Split into subdirectories only when a header starts including another project header, when one
  role outgrows ~3 files (the likely first case: the `type ==` dispatch chains in `app.cpp` become
  `backends/<type>.cpp` behind a catalog interface), or when a second target reuses part of `src/`.

Key architectural facts:

- **Single-threaded dispatch.** mcp-cpp runs tools on one thread, so `App` members are plain
  (no locking). `App::db(name)` returns the backend or `nullptr` — every tool validates `db` first and
  the error lists the available backend names (self-correcting design).
- **MRTR is centralized.** All writes funnel through `stageWrite(ctx, kind)` (in `data_tools.cpp`,
  reused by `sql_tools.cpp`). First call returns `input_required` + `requestState` and stages a
  `PendingOp`; an approved retry echoes `requestState` + `inputResponses:{confirm:{approved:true}}`
  and runs `executeWrite`. `--yes` bypasses the gate (tests/automation only); `--allow-sql` is what
  registers `db_exec_sql`. Client confirmation payloads are shape-tolerant (`confirm`/`content`/
  `value`/`params`, `action: accept|decline`) — see `confirmApproved` in `data_tools.cpp`.
- **Metadata comes from catalogs, never from querying rows** (the demo's "guess columns by querying"
  fails on empty tables): sqlite `pragma table_info` (via a TVF — a bare `pragma` breaks zorm's
  ` limit 1` column probe), `information_schema`, `user_tab_columns`, or the jsonfile columns array.
  Add a backend's catalog branch in `app.cpp`, not in a tool handler.
- **Resources are registered as URI templates**, matched at read time, so tables created at runtime
  are readable without a server restart (deliberately not a startup snapshot).
- **`instructions` is the primary routing channel** (connect-time decision table); tool descriptions
  carry "use me when / prefer X" hints; error messages teach (cheapest self-correction loop);
  `db://guide` + `db-playbook` are the pull-based fallback for clients without instructions support.
  Keep new wording in `guide.cpp` so it stays coherent.

## zjson gotchas (encoded in `json_util.hpp` — read it before touching JSON handling)

- `member(json, key)` returns `Json(nullptr)` for a missing key, and zjson renders Null as the literal
  `"null"` — test presence with `isNull()`/`isError()`/`hasChild`, never `toString().empty()`.
- `json[key]` **deep-searches nested layers** when the key is absent at the top level — structured
  reads must go through `hasChild`, never `[key]` blindly.
- Drivers fold unquoted identifiers to upper case on some backends (dm8), so use `rowVal(row, {"a","A"})`
  with candidate key lists.
- Status codes: `200` and `202` are both success (`202` = empty result, returned by jsonfile); judge
  via `okStatus`, not `== 200`.
- `mcp::Json` currently aliases `ZJSON::Json`, but the bridge deliberately round-trips through wire
  text to keep the two layers independently replaceable.

## Launcher, config, and the cwd trap

MCP clients each choose the child's cwd (the protocol specifies nothing), and without `--config` the
default DB files (`zorm-mcp.db` / `zorm-mcp.json`) land in the **process cwd** — so register `start.sh`
(or `start.cmd`), which pins cwd to the repo root and execs `bin/zorm_mcp_server`. Even then, prefer
`--config <absolute path>`: a relative `connString` in the config resolves against the **server's**
cwd (the repo root), not the AI client's project directory. `zorm-mcp.json` is the five-backend
template.

## Windows-only pitfall: `SQLITE_READONLY` on write, reads fine

Not a server bug — Claude Code runs MCP children at **low integrity**, which can read medium-integrity
files but not write them, and SQLite silently degrades to a read-only open until a write is attempted.
Fix with `icacls <path> /setintegritylevel Low` on the DB file and its directory. Full walkthrough in
`demo/README_demo.md` §6.

## Client integration docs

`INTEGRATION.md` is the per-client configuration manual (Claude Code / Desktop, DSH, VS Code/Copilot,
Cursor, Windsurf, Cline/Roo, Codex CLI, Gemini CLI, MCP Inspector, raw HTTP) with verified-vs-placeholder
status. `README.md` is the tool table + MRTR + CLI-flag reference. Update both when the tool surface
changes.
