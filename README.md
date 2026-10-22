# zorm-mcp — 多后端数据库操作 MCP（正式版）

**一句话**：一个 MCP server，让 AI 用**同一套结构化语义**操作 sqlite / jsonfile / mysql / postgres / dm8 五种后端——不写一行 SQL 方言；写操作全部经过 MRTR 人机确认。

由 `demo/`（Zorm Analytics MCP 演示）验证过的架构升级而来：

| | demo（验证版） | zorm-mcp（本目录，正式版） |
| --- | --- | --- |
| 工具 | 6 个 | **13 个**：Idb 八接口全覆盖 + 表结构/索引/版本元数据 + 参数化只读 SQL + 事务 |
| 表结构获取 | 查表猜列（空表失效） | **catalog 元数据**（pragma table_info / information_schema / user_tab_columns / jsonfile columns 数组），空表也返回完整列定义 |
| AI 理解 | 基础描述 | 分层设计：instructions 决策表 + 工具路由提示 + 自纠错报错 + playbook prompt/资源 |
| 代码形态 | 单文件 1700 行 | 多文件模块化（app / guide / meta / data / sql / resources） |

## 构建

```bash
git submodule update --init --recursive   # 首次：拉取 thirds/ 下的第三方项目
cmake --preset clang-rlz                  # Release（默认）
cmake --build build -j 8
ctest --test-dir build                    # e2e 测试（真实 stdio 子进程）
# build/ 只放中间产物；最终可执行文件与运行 DLL 落在 bin/
# DLL 按依赖闭包解析（后端 DLL + libc++/libmariadb 及其传递依赖），客户端无需 MSYS2 PATH
```

### 第三方依赖（`thirds/`，git submodule）

| 子模块 | 内容 |
| --- | --- |
| `thirds/zorm` | zormlib + 后端 DLL + zjson（zorm 项目） |
| `thirds/mcp-cpp` | MCP 协议库 **v0.4.0**（mcp-cpp 项目）：2026-07-28 协议、双时代（legacy/stateless）、MRTR 封装、Tasks 扩展、HTTP 鉴权 seam、多 worker 调度 |

两者都是**独立项目**（各自有远端仓库）：要改 zorm / mcp-cpp，直接进对应子模块目录改并提交推送（那里就是完整仓库），然后回到本项目记录新版本：

```bash
cd thirds/zorm && git add -A && git commit -m "..." && git push   # 在子项目里改
cd ../.. && ./sync-thirds.sh      # 拉取最新 master 并提交子模块指针变更
```

`sync-thirds.sh` 等价于 `git submodule update --remote --merge thirds/zorm thirds/mcp-cpp` 加上自动提交指针变更。子模块已在 master 分支上跟踪各自远端（`.gitmodules` 的 `branch = master`）。

零依赖开箱即用：不配置时内置 `sqlite3`（当前目录 `zorm-mcp.db`）与 `jsonfile`（`zorm-mcp-data.json`）两个后端。

> `zorm-mcp.json` 是**五后端配置模板**，不是数据文件。默认数据文件叫 `zorm-mcp-data.json`；如果把配置模板误配成 jsonfile 的 `connString`，服务器会在启动时明确拒绝并退出，避免配置被当作数据库覆盖。

## 接入 AI 客户端

**👉 全客户端配置手册见 [INTEGRATION.md](INTEGRATION.md)**——覆盖 Claude Code / Claude Desktop / DeepSeek Harness / VS Code & Copilot / Cursor / Windsurf / Cline & Roo Code / Codex CLI / Gemini CLI / MCP Inspector / 通用 HTTP，含占位待验证客户端清单。

快速接入（统一走 start 脚本，把数据库文件钉在本目录；**推荐带 `--config` 显式指定数据库**，见 [INTEGRATION.md §0.2](INTEGRATION.md) 的原因说明）：

```bash
# Claude Code（最简：内置 sqlite3 + jsonfile，默认库文件落在本目录）
claude mcp add zorm-mcp -- D:/codes/zorm-mcp/start.sh

# 推荐：--config 显式指定数据库（模板见 zorm-mcp.json，connString 写绝对路径）
claude mcp add zorm-mcp -- D:/codes/zorm-mcp/start.sh --config D:/codes/zorm-mcp/zorm-mcp.json
```

```jsonc
// 其他客户端通用形态（各客户端配置文件位置见 INTEGRATION.md）
{ "mcpServers": { "zorm-mcp": {
    "command": "D:/codes/zorm-mcp/start.sh",
    "args": ["--config", "D:/codes/zorm-mcp/zorm-mcp.json"]
} } }
```

## 工具一览（全部带 `db` 路由参数 = 后端注册名）

| 工具 | 写? | 对应 Idb | 说明 |
| --- | --- | --- | --- |
| `db_list_tables` {db} | | — | 用户表清单 |
| `db_describe_table` {db, table} | | querySql(内部) | **表字段元数据**：列名/类型/可空/默认值/主键，空表可用（catalog 直查，不靠查表猜列） |
| `db_indexes` {db, table} | | querySql(内部) | 索引清单（各方言 best-effort） |
| `db_info` {db} | | querySql(内部) | 后端类型 / 服务器版本 / 连通性 |
| `db_query` {db, table, …} | | select | 智能查询：fields / lks / ins / ors / group / sum / count / sort / page / size + 列条件（`{price:">,100"}` 比较前缀）。`ins:{age:[18,20]}` = 单列多值；`count/sum:"col,alias"` 或 `{col:alias}`；`size` 可单独用（默认 page=1），`page` 必须与 `size` 同时给 |
| `db_count` {db, table, where?} | | select | 计数 |
| `db_query_sql` {db, sql, values?} | | querySql | 参数化**只读** SQL（SELECT/WITH/PRAGMA/SHOW/EXPLAIN；JOIN/方言函数/EXPLAIN 的出口）。`values` 用 `?` 占位符；postgres 由服务端自动改写为 `$1..$n`。允许前置注释，仍拒绝多语句。**WITH/EXPLAIN 会检查语句体**：`WITH … DELETE/UPDATE/INSERT` 与 `EXPLAIN ANALYZE <写操作>` 会真的落库，一律拒绝（2026-10-22 实地报告 F1） |
| `db_insert` {db, table, data} | ✅MRTR | create | 单行插入（缺 id 自动生成） |
| `db_insert_batch` {db, table, rows, constraint?} | ✅MRTR | insertBatch | 多行 upsert（冲突列默认 id） |
| `db_update` {db, table, data, where:{id}} | ✅MRTR | update | 单行更新 |
| `db_delete` {db, table, where} | ✅MRTR | remove/execSql | `{id}` 删一行；`{}` 清全表（确认文案带行数预估） |
| `db_trans` {db, sqls} | ✅MRTR | transGo | 原子事务：`{text|sql, values}` 或结构化 `{table, method, params, id}` |
| `db_exec_sql` {db, sql} | ✅MRTR | execSql | 任意写/DDL SQL，**`--allow-sql` 才注册** |

配套 MCP 能力：

| 资源 | 形态 | 说明 |
| --- | --- | --- |
| `db://guide` | 固定 | 完整用法手册（拉取层兜底） |
| `db://schema` | 固定 | 全部后端总览 |
| `db://schema/{db}` | **URI 模板** | 该后端的表清单 |
| `db://schema/{db}/{table}` | **URI 模板** | 单表完整列元数据（type/nullable/pk），空表同样可用 |

参数化 URI 以 **URI 模板**（`resources/templates/list`）注册，读时按模板实时匹配——**运行期新建的表无需重启 server 即可读取**（不是启动快照）。另有 `db-playbook` prompt（用法手册）与 completion（db/table/fields 补全）。

## 让 AI 用对工具：四层理解设计

1. **推送层（主）**：`instructions` 决策表随连接注入模型上下文；每个工具 description 写明“何时用我 / 何时该用别的工具”，schema 属性带语义说明。
2. **自纠错层**：报错即教学——unknown db 附可用后端清单、不支持的条件给替代方案、只读 SQL 拒绝时指路写工具。
3. **拉取层（兜底）**：`db-playbook` prompt 与 `db://guide` 资源承载完整手册（不支持 instructions 的客户端用）。
4. **schema 即文档**：`db_describe_table` + resources 让 AI 不猜列名。

## 标识符校验与安全边界

结构化工具（`db_query` / `db_count` / 所有 CRUD / `db_trans` 的结构化元素）只接受**简单标识符**：

- 表名、列名、`fields`、`group`、`sort` 列、`lks/ins/ors` 键、`sum/count` 列与别名、`data` 键、批量 `constraint`：字母/数字/下划线，不能以数字开头。
- `sort` 只允许 `col [asc|desc][, ...]`；非法输入在构造 SQL **之前**返回 `InvalidParams`，错误信息带 `invalid ... name`。
- 复杂对象名（含空格、连字符、引号、schema 前缀）请用 `db_query_sql`（或 `--allow-sql` 下的 `db_exec_sql`）显式书写。
- `db_query_sql` / `db_exec_sql` / `db_trans` 的原始 SQL 文本是显式逃生口，不做标识符校验——这是设计选择，不是遗漏。

## MRTR 写确认

所有写工具首次调用**不执行**：返回 `resultType=input_required` + `requestState`，确认文案含 目标库/表/影响行数。客户端（AI 转述给用户）批准后，带原参数重试，并在 **tools/call 的 params 层**（与 `arguments` 同级）回传：

```jsonc
"params": {
  "name": "db_insert",
  "arguments": { /* 原参数 */ },
  "requestState": "v1.…",                       // 原样回传，勿修改
  "inputResponses": { "confirm": { "approved": true } }  // false = 取消
}
```

`requestState` 由 mcp-cpp 0.4 的 `RequestStateCodec` 做 HMAC 封装：绑定到当前工具（HTTP 模式还绑定认证主体），伪造、跨工具重放、过期都会被拒绝——此时重新发起一次写调用即可重新 stage。对客户端回传的确认形态仍做了宽泛容错（`confirm`/`content`/`value`/`params` 嵌套或顶层 `approved`，以及 `action: accept/decline`）。

**降级批准方式（仅限无法在 params 层回传的客户端）**：把原参数重发一次，并在 `arguments` 对象里加 `"inputResponses":{"confirm":{"approved":true}}`（**不带** `requestState`）。服务器会回退到该 `(db, table, kind)` 最近一次 pending 操作。这条路径没有 state 完整性校验，只应用于受信的本机客户端。

## 协议与传输（mcp-cpp 0.4）

- **双时代**：同时服务 2026-07-28 stateless 协议（`server/discover`，每请求 `_meta`）与 legacy `initialize`（2024-11-05 … 2025-11-25）。
- **CacheableResult**：`*/list` 与 `resources/read` 自动注入 `ttlMs`/`cacheScope`（严格客户端如 DSH / MCP SDK 2.0 必需）；每条 modern 结果带 `_meta.serverInfo`。
- **资源通知**：写操作后发布 `notifications/resources/updated` 与 `notifications/resources/list_changed`，并在 capabilities 中声明 `resources.subscribe` / `listChanged`。
- **MRTR state**：HMAC 封装 + 工具绑定；HTTP 下额外绑定 Bearer principal（见上节）。
- **派发模型**：库默认 `MCP_WORKERS=1`（单线程），本 server 的 `App` 状态按此设计为无锁；如改用多 worker，需先让全部 handler 线程安全。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--config <file>` | 多后端配置（`{"databases": {名字: {type, options}}}`）；缺省内置 sqlite3 + jsonfile |
| `--http <port>` | Streamable HTTP 模式（`http://127.0.0.1:<port>/mcp`），默认 stdio |
| `--http-token <bearer>` | HTTP 静态 Bearer 鉴权，可重复（多个 token）。未带 `Authorization: Bearer …` 的请求返回 401；token 同时作为 MRTR 状态绑定的 principal |
| `--http-origin <origin>` | 只允许该 `Origin`（大小写不敏感；无 Origin 的非浏览器客户端仍放行） |
| `--sse-keepalive <ms>` | 订阅 SSE 流保活间隔，默认 5000；`0` 关闭；`1–99ms` 与 `> 1h` 会被判为错误配置 |
| `--yes` | 关闭 MRTR 确认（自动化/测试专用） |
| `--allow-sql` | 暴露 `db_exec_sql`（默认隐藏） |
| `--child` | 内部参数：e2e 测试孵化用 |

## 测试

```bash
ctest --test-dir build --output-on-failure   # 全部 3 个套件
ctest --test-dir build -R mcp_test_zorm_mcp -V   # e2e：真子进程（stdio + HTTP）
```

| 套件 | 覆盖 |
| --- | --- |
| `mcp_test_zorm_mcp` | e2e：元数据 / 智能查询 / 只读 SQL 闸门 / 批量 / 事务 / MRTR（含篡改、跨工具、跨 principal、一次性）/ 协议双时代 / 资源订阅 / HTTP Bearer / 配置与 CLI |
| `mcp_test_zorm_unit` | 纯函数单测：JSON 桥、结果信封、`rowVal`、标识符白名单、guide/playbook 文案 |
| `mcp_test_zorm_analytics` | demo 展示服务器的完整剧本 |

覆盖率（Clang + llvm-cov，可选构建）：

```bash
cmake --preset clang-rlz -B build-cov -DZORM_MCP_COVERAGE=ON
cmake --build build-cov -j 8
LLVM_PROFILE_FILE="$PWD/build-cov/prof-%p.profraw" ctest --test-dir build-cov
llvm-profdata merge -sparse build-cov/*.profraw -o build-cov/zorm.profdata
llvm-cov report bin/zorm_mcp_server.exe -instr-profile=build-cov/zorm.profdata
```

远端方言矩阵（mysql / postgres / dm8，默认跳过，CI 无需远端）：

```bash
ZORM_MCP_REMOTE_CONFIG=D:/codes/zorm-mcp/zorm-mcp.json \
  bin/mcp_test_zorm_mcp.exe --gtest_filter=ZormMcpE2E.RemoteBackendsMatrix
```

`./run-test all` 仍是 zorm 核心库的回归门（改动 `thirds/zorm` 时必须跑）。五后端差异表见 [docs/dialects.md](docs/dialects.md)。

## 常见问题

- **写入报 `SQLITE_READONLY` 但读正常**：Windows 完整性级别问题（Claude Code 以低完整性孵化 MCP 子进程），不是 server bug。修复见 `demo/README_demo.md` 第六节（icacls 降级目录与文件标签）。
- **`db` 参数写错**：报错会列出全部可用后端名，按提示改正（是 `sqlite3` 不是 `sqlite`）。
- **mysql/pg/dm8 连不上**：确认对应数据库服务器在运行、DLL 在 exe 同目录（构建时自动复制）。
