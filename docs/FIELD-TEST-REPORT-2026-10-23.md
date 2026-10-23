# zorm-mcp 实地测试报告（2026-10-23）

> 测试对象：`bin/zorm_mcp_server.exe`（2026-10-23 构建，v1.1.1，`main.cpp` 版本号）
> 测试方式：**真实客户端 + 裸协议双通道**。测试人即 AI 客户端——本报告撰写时测试环境
> 本身就是以 [INTEGRATION.md] 记载的 VS Code/Copilot 方式注册接入 zorm-mcp 的（`.mcp.json`
> → `cmd.exe /c start.cmd`，无 `--yes`、无 `--allow-sql`，标准 12 工具形态），因此“读路径”
> 的每一条结论都来自真实客户端实地调用；“写/MRTR/协议细节”走自研 stdio JSON-RPC harness
> （`.tmp-field/ft3/mcp_client.py`，直接起服务器子进程逐行收发 JSON-RPC）。
>
> **总判定：通过。** 7 组测试共 **283 项断言全部通过**（runner1 35 + runner2 97 + runner3 40
> + runner4 37 + runner5 17 + runner6 45 + runner7 6，外加真实客户端 8 次调用抽查），
> 另沉淀 **10 条发现**（无 Critical/High，见 §6）。修复回归见 §6.1（298 项断言全绿）。

## 1. 测试环境

| 项 | 值 |
| --- | --- |
| 平台 | Windows（MSYS2 clang64 构建产物），`bin/` 含 DLL 闭包（无需客户端 PATH） |
| 本地后端 | sqlite 3.40.1（内置默认）、jsonfile（内置默认）、均为独立文件 |
| 远端后端 | MySQL 8.3.0、PostgreSQL 14.12、DM8（DM Database Server 64 V8）@10.0.0.7/dbtest |
| 测试面 | stdio（MCP 2024-11-05 → 2026-07-28 五个协议版本）+ Streamable HTTP（`--http N`） |
| 工具 | 全部 13 个（`db_exec_sql` 在 `--allow-sql` 下注册，默认 12 个） |

## 2. 实测覆盖矩阵（13 工具 × 5 后端）

| 工具 | sqlite3 | jsonfile | mysql | postgres | dm8 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| `db_list_tables` | ✓ | ✓ | ✓ 12表 | ✓ 16表 | ✓ 8表 | dm8 按 owner=dbtest 过滤实测正确 |
| `db_describe_table` | ✓ 空表可用 | ✓ | ✓ | ✓ | ✓ | 均来自 catalog，与行无关；大小写键兼容（dm8 返回大写） |
| `db_indexes` | ✓ | ✓ `no index concept` | ✓ SHOW-INDEX 字段透传 | 未跑（远端） | ✓ | jsonfile 明确注明无索引概念 |
| `db_info` | ✓ 3.40.1 | ✓ ready | ✓ 8.3.0 | ✓ 14.12 | ✓ DM V8 | |
| `db_query` | ✓ | ✓ | ✓ | ✓ | ✓ | 等值/比较前缀/`lks`/`ins`/`ors`/`fields`/`sort`/`page`/`size`/`group+sum+count` 全部五后端语义一致 |
| `db_count` | ✓ | ✓ | ✓ | ✓ | ✓ | 信封 `data[0].total` 一致；发现 F-1（见 §6） |
| `db_query_sql` | ✓ | ✓ 仅 select..from | ✓ EXPLAIN | ✓ `?`→`$n` | ✓ | 见 §3/§5 |
| `db_insert` | ✓ | ✓ | ✓ | ✓ | ✓ | 缺 id 自生成 8 位 hex；同 id 覆盖（upsert）语义五后端一致 |
| `db_insert_batch` | ✓ | ✓ | ✓（e2e 面） | ✓（e2e 面） | ✓ | upsert 且不返回单条 insertId |
| `db_update` | ✓ | ✓ | ✓ | ✓ | ✓ | 仅 `where:{id}`：条件更新被明确拒绝并指路 `db_trans` |
| `db_delete` | ✓ | ✓ | ✓ | ✓ | ✓ | 同上；`{}` 清全表带影响行数 |
| `db_trans` | ✓ | ✓ | ✓ | ✓ | ✓ | 文本+结构化混合事务、失败原子回滚；dm8 见 F-8 |
| `db_exec_sql` (`--allow-sql`) | ✓ | n/a* | ✓ | ✓ | ✓ | *jsonfile 本无 SQL 写入口（`Unsupported SQL` 明确报错） |

## 3. 关键断言实录

### 3.1 协议面（runner1，35 项）
- `initialize` 在 **2026-07-28 / 2025-11-25 / 2025-06-18 / 2025-03-26 / 2024-11-05** 五个版本全部正确协商，`instructions`（2,642 字节决策表）随连接下发；`serverInfo = zorm-mcp v1.1.1`。
- `capabilities`：`resources {listChanged:true, subscribe:true}`、`tools/prompts/completions` 齐备。
- 资源：固定 `db://guide`（3,628 字节手册）+ `db://schema`；**URI 模板** `db://schema/{db}`、`db://schema/{db}/{table}` 经 `resources/templates/list` 返回；未知 db/table 的模板读取返回“教学型”错误（附可用后端清单）。
- `db-playbook` prompt 与 `completion/complete`（`db:'sql'` → `["sqlite3"]`，按前缀过滤表名）均可用。
- 未知方法 → `-32601`；**垃圾行灌入 stdin 不崩溃**、stderr 保持零污染（stdio purity）、EOF 优雅退出 code 0。
- legacy(2024) 客户端 `resources/subscribe` 被接受（mcp-cpp stub，无 per-URI fan-out，见 F-7）；2026(io.modelcontextprotocol 时代) `subscriptions/listen` 订阅后**:首消息必须为 ack**，随后每次写入收到 `notifications/resources/updated`（附 `uri` + `_meta."io.modelcontextprotocol/subscriptionId"`）与 `list_changed`。jsonfile 写入同样触发。

### 3.2 数据工具语义（runner2，97 项，sqlite3+jsonfile 双后端）
- 空库列零表；catalog 直查**空表也有完整列元数据**（pk/nullable/type/default）。
- `db_query`：顶层列条件（等值 + `">,20"` 比较前缀）、`lks` 子串、`ins:[v1,v2]`、`ors` OR、`fields` 投影、`sort` asc/desc、`size` 单独给=第一页、`page+size` 分页带 `records/pages`、`group+sum+count` 聚合（别名统一 JSON 数字）在两后端结果一致。
- 写：缺 id 自动生成（8 位 hex，实测 `868e3ee2` 形态）；**db_insert 同 id 二次写入为 upsert**（runner3 用同 id 四次插入收录为 1 行验证）；batch upsert 后 count 正确；条件更新/删除被明确拒绝并指路 `db_trans`（文案教学式）。
- **跨重启持久化**：两后端数据与 schema 重启后完整可读（jsonfile 文件重载，sqlite 重新缓存）。
- jsonfile 弱类型：字符串 `'2'` 命中整型 `2`；emoji/中文 round-trip 正常。
- jsonfile 原始 SQL **只支持 `select … from <table>`**：WHERE/聚合/未知表全部**明确报错**并指路 `db_query`（不再出现旧版“202 空结果”的假阴性）。
- `where:{}` 清全表后：表仍列在 `db_list_tables`，count=0，select 返回 202 —— 空结果语义清晰。

### 3.3 只读 SQL 闸门（`db_query_sql`，statement-level）
| 输入 | 结果 |
| --- | --- |
| `select … where …` + `values` 绑定 | ✓（postgres 自动改写 `?`→`$1..$2` 实测通过） |
| 注释环绕的 SELECT（`-- probe` / `/* trailing */`） | ✓ 放行 |
| `update` / `insert` / `delete` / `select 1; select 2`（多语句）/ 尾随注入 | ✗ 拒绝 |
| `with c as (select 1) delete from items`（数据修改型 CTE） | ✗ 拒绝 |
| `explain analyze create table zz as select * from items` | ✗ 拒绝（EXPLAIN 只能包 SELECT/WITH/VALUES） |
| `pragma user_version = 42` | ✗ 拒绝（赋值形态需 `db_exec_sql`） |
| `pragma table_info(items)`（功能型） | ⚠ 返回 `[{}, {}]` 空对象行 → **F-2**，TVF `select * from pragma_table_info('t')` 为工作的替代（已在 docs 记载） |

### 3.4 MRTR 写确认闸门（runner3，40 项；runner7 补 HTTP principal 绑定）
- 默认（无 `--yes`）所有写（含 `db_exec_sql`）先进 `'Confirm insert on sqlite3.t (1 row)? …'` 的 `input_required` 结果：`requestState`（`v1.<b64>.<mac>` 封签）、`resultType:"input_required"`、`inputRequests.confirm = elicitation/create`（boolean `approved`）齐备；**staged 期间数据未写**。
- 批准重试：`requestState + inputResponses.confirm.approved=true` 放在 **tools/call params 层**（`arguments` 兄弟位）执行成功；4 种兼容批准形状（扁平/`content`/`value`/`params`）与 `action:"accept"` 均识别。
- 拒绝路径返回 `"Write cancelled by user."`（fail-safe：无请求状态时撤销最近匹配的待处理 op，不执行任何写入）。
- 对抗性全部拒绝：**伪造**状态、**篡改 seal**（HMAC 失败）、**跨工具**状态（insert 的 state 用于 update）、**已消费**状态重放、`requestState` 放进 `arguments` 内部（提示正确放置位置）；数据未受任何污染（各步骤 count 断言）。
- 无状态回退：客户端丢失 state 仍回传 confirm 时，回落到同 (db,table,kind) **最新**待处理 op；更旧的 op 已被重新暂存（re-stage）淘汰 → `no pending insert op`。
- `--yes`：写入直接执行（insert/delete-all 实测），符合“automation only”设计。
- HTTP 多 bearer：principal A stage → principal B 重放 → 拒绝（`...issued for a different tool`）；A 本人重试执行 —— **principal 绑定实测有效**；`--http-token` 可重复注册多 token（tk-ALPHA/tk-BETA 双双 200）。

### 3.5 HTTP 传输（runner5，17 项）
- 无 token 配置：开放匿名（`HTTP serving on .../mcp (anonymous)`）。
- 配置 token：无 Authorization/Wrong → 401；合法 bearer → 200；`--http-origin http://good.example` 时 `Origin: http://evil.example` → **403**，合法 origin → 200。
- 无会话 `GET /mcp` → 405；畸形 JSON body → 400 且服务器存活（后续请求正常）。
- 单发 POST initialize 用 application/json 应答（spec 允许的单次响应形态）。

### 3.6 部署与健壮性
- **干净环境**（`SystemRoot/TEMP` 级最小 env，无 PATH）下 stdio 冷启动 → `db_info ready`：**DLL 闭包打包实测有效**（客户端不需要 MSYS2）。
- cwd 陷阱验证：无 `--config` 从新 cwd 启动 → `zorm-mcp.db` 起始即建；jsonfile 存储文件**首次实际写入行时**才物化（此前仅内存空表）。
- 配置误用防线：把五后端模板 `zorm-mcp.json`（其 jsonfile connString 即模板自身）作为配置启动 → 启动期 `refusing` + 非零退出，模板不会被当数据库污染。
- 未知 CLI flag → usage 文案 + 退出码 2；任务扩展未启用时 `tasks/*` 干净地 `-32601`。

## 4. 远端矩阵实测（runner6，45 项，10.0.0.7 真服务器）

- 三后端 `db_info` ready；DM8 报 `DM Database Server 64 V8`。
- 全套结构化语义（含比较前缀、lks、事务、回滚原子性、describe/索引）在 mysql/pg/dm8 与本地一致；**postgres `?`→`$n` 自动改写实测**（`where id = ?` 绑定正确返回结果）。
- mysql `EXPLAIN` 走 `db_query_sql` 正常（历史 D-08 修复传承面可用）。
- dm8 要点现场证据：
  - 结构化工具链（db_insert/db_query/db_count/db_describe/db_indexes/transGo）全部正常；
  - **raw DDL 的命名规则**（F-3）：`create table ft3_probe …`（不加引号）`status:200` 但落在 `SYSDBA.FT3_UNQ2`（登录用户 schema、大写折叠），对以 `"dbtest"` 限定的工具面不可见；必须写 `create table "dbtest"."ft3_probe" ("id" varchar(32) primary key, …)`（与 zorm 生成侧的 quoted-lowercase 完全对齐）；
  - **事务回滚原子性**确认：夹杂不存在表的 trans 失败后 count 不变。
- dm8 错误文本 GBK→UTF8 转换**跨调用/跨会话稳定**（6/6 样本 `第1 行附近出现错误: 无效的表或视图名[…]`），run6.out 里的乱码系重定向编码所致（已复核 harness 侧 artifact）。

## 5. 与文档宣称的对照

| 宣称（README/docs/CLAUDE.md） | 实测 |
| --- | --- |
| 13 工具 5 后端一套语义 | ✓（§2 矩阵） |
| catalog 元数据、空表可用 | ✓ |
| instructions 四层理解设计 | ✓ instructions 2,642B；工具 description 带路由提示；错误自纠错（unknown db/table/条件均有教学式文案）；`db://guide`+`db-playbook` 兜底可用 |
| MRTR：integrity 保护 + 客户端形状宽容 | ✓（§3.4，forged/cross-tool/consumed/cross-principal 全拒） |
| `--yes` 仅自动化 | ✓ |
| db_query_sql 只读闸门防线清单 | ✓（§3.3，含 EXPLAIN-Analyze-CTA/PRAGMA 赋值的“真的会执行”类拒绝） |
| 标识符校验在构造 SQL 之前 | ✓ 8 类非法名全部 `Invalid invalid … name`（含 sort 注入、`tot'al` 别名、`qty'; --` group） |
| lks/ins/ors 值含逗号显式拒绝并指路 `db_query_sql` | ✓ |
| post-greSQL 占位符自动改写 | ✓ |
| jsonfile SQL 子集 | ✓ |
| 结果信封归一（affected/affectedRows、dm8 数字化聚合、多行 batch 无单 insertId、`[file: …]` 仅文件后端） | ✓ |
| URI 模板运行期可读新表 | ✓（创建 `rtime` 表后不重启直接 `resources/read` 成功） |
| 写入通知（list_changed+updated+subscribe 能力声明） | ✓（2026 fan-out；legacy stub 见 F-7） |
| DLL 闭包免 PATH | ✓（干净 env） |
| cwd 陷阱与 `--config` 绝对路径建议 | ✓（默认文件落进程 cwd；重定向至 WORK 时 jsonfile 以 cwd 相对解析） |
| 配置模板自污染防线 | ✓（refuse + exit） |

## 6. 发现清单（按优先级）

| # | 级别 | 位置 | 现象与证据 | 建议 |
| --- | --- | --- | --- | --- |
| F-1 | P3 参数卫生 | `data_tools.cpp toolCount` | `db_count` 会**静默忽略** `where` 之外的未知顶层键：`{db,table,qty:999}` 返回全表计数（4），不像 `db_query` 那样把未知键当列条件或显式报错——AI 有理由误信条件已生效（.tmp-field/ft3/run2 复现） | 与 `db_query` 对齐：未知顶层键报 `InvalidParams`（或要求必须写 `where`） |
| F-2 | P3 可诊断性 | zorm sqlite 取数 + 门 | 通过 `db_query_sql` 调功能型 PRAGMA（`pragma table_info(t)`、`pragma mmap_size`）返回**全空对象行** `[{},{}]`，数据列丢失；TVF 形式（`pragma_table_info('t')`）正常，项目 catalog 亦因此用 TVF | 在 `checkReadOnlySql` 放行时对功能型 `pragma` 提示改用 TVF，或在 zorm 修复空列名折叠 |
| F-3 | P3 方言约束 | dm8 raw DDL | 裸表名 `create table ft3_x`：`status:200` 但落到 `SYSDBA.FT3_X`（大写、登录 schema）；工具面以 `"dbtest"` 限定→**不可见的静默成功**；正确写法 `create table "dbtest"."ft3_x" ("id" …)`（列也必须带引号小写） | `docs/dialects.md` §5 补一句“dm8 裸 DDL 必须 `"dbtest"."表"` 且列名带引号小写”；条件允许时对 dm8 raw DDL 的裸名给 stderr 警示 |
| F-4 | P4 文档差异 | `db_query` sum/count | 数组形式 `count:["*"]` 展开为 `*,*` → SQLite `near "*": syntax error`；字符串形式 `count:"*,n"` 正常。README 称三形式皆可 | 文档注明：数组形式仅适合普通列名；`*` 请用字符串对形式 |
| F-5 | P4 观感 | `db_exec_sql` | 对 SELECT 报 `affected:1` 且无数据行（zorm execSql 语义），AI 可能误读为“影响 1 行” | 工具 description 注明“只回影响行数，读数据用 db_query_sql”（已有近似文案，可补一句） |
| F-6 | P4 CLI 校验 | `--sse-keepalive` | 非 ASCII 数字串如 `bogus` 被 `atoi` 吞成 0（=禁用 keepalive）继续启动，无告警（`--http` 路径实测继续 serving） | 参数解析期做数字校验，非法即 exit 2 |
| F-7 | P4 设计注记 | mcp-cpp legacy stub | legacy Era `resources/subscribe` 返回成功但**无** per-URI 通知（mcp-cpp 0.4 明示由 transport 自行实现）；只有 2026 `subscriptions/listen` 有真实 fan-out | 在 INTEGRATION.md 中标注 legacy 客户端订阅为“空 stub”，避免误导 |
| F-8 | P3 可诊断性 | dm8 transGo 错误回传 | 事务内失败时错误文本为空（`Running transaction error: `，重复案例：不存在表/重复键）；单语句路径错误正常带 GBK→UTF8 转换回传 | zorm dm8 trans 路径透传驱动错误消息（“科学斯基: 子模块侧修复，重跑 `./run-test all`”） |
| F-9 | P3 UX | zorm create 错误文案 | 无 id 自动生成的 8-hex 字符串命中 `INTEGER PRIMARY KEY` 表时，`Error Code : 20`（SQLITE_MISMATCH）文案晦涩不可自纠 | zorm 侧透出 sqlite 明细（"datatype mismatch"）或文档加粗“id 列应为文本类型” |
| F-10 | P4 文案 | `inputRequiredConfirm` | 确认文案 `"1 statements"` 单复数瑕疵 | 微调 |

> F-1/F-8/F-9 建议进近期的改进列表；其余为文档或 5 分钟级小项。无安全类发现。

### F-11（手册复现途中新发现，p3，已修复）

撰写 [HANDS-ON-TEST-GUIDE.md](HANDS-ON-TEST-GUIDE.md) 并按其五后端逐项核对时发现：
F-4 的 `*`→`total` 别名默认只改了**生成端**（`convertStringLists`），`aggregateAliases`
（dm8 字符串聚合→数字归一化，D-12）未同步，导致 dm8 上 `count:"*"/["*"]/{ "*":"" }`
返回 JSON **字符串** `"3"` 而非数字。`data_tools.cpp aggregateAliases` 已对齐同一默认别名
（一行改动），**五后端 15×5=75 项语义核对 + 全部 9 套回归 318 项断言 0 失败**（见 §6.1 与
`.tmp-field/ft3/probe11_guidecheck.py`）。

## 6.1 修复回归（2026-10-23 下午，提交 dcefe72/dcefe71 审核后追加）

提交 `dcefe71`（"Address the 2026-10-23 field report findings (F-1 to F-10)"，
含 `thirds/zorm` 指针 `0a4b812 → 01d8e48`）已针对本清单修复并补充 e2e
（`ZormMcpE2E.FieldReport20261023Findings`）。审核结论与独立回归结果如下。

### 审核结论（代码 diff + 子模块 commit 01d8e48）

| 项 | 处置 | 审核意见 |
| --- | --- | --- |
| F-1 | `toolCount` 未知顶层键 → `InvalidParams`（点名键并给出 `where` 形状）| ✔ 正确；白名单式（db/table/where + 3 个协议重试字段），不破坏 MRTR 重试 |
| F-2 | zorm `01d8e48`：功能型 PRAGMA 从语句取列名，行数据恢复 | ✔ 正确；TVF 与旧路径均保留 |
| F-3 | 文档化（`docs/dialects.md` §5） | ✔ 措辞准确（裸名落登录 schema + 大写折叠，列亦须引号小写） |
| F-4 | 匿名 `*`（字符串/数组/`{col:""}`）别名 `total`；显式别名不变 | ✔ 实现最小且向后兼容 |
| F-5 | `db_exec_sql` description 增补 affected-count 语义 | ✔ |
| F-6 | `--http`/`--sse-keepalive` 严格数字解析，非法 exit 2；含上界（端口 1..65535、keepalive ≤86400000ms）| ✔ 比建议更进一步（范围检查） |
| F-7 | 文档化（INTEGRATION.md 故障排查节） | ✔ |
| F-8 | zorm dm8 transGo 透传驱动诊断文本 | ✔ 错误文本非空且 GBK→UTF8 正常 |
| F-9 | zorm sqlite 错误附驱动明细（`Error Code : 20 (datatype mismatch)`）| ✔ 自纠错闭环 |
| F-10 | 确认文案单复数（`1 statement` / `2 statements`） | ✔ |

**F-11（后记）**：手册复现核对发现 `aggregateAliases` 未同步 F-4 的 `*`→`total` 默认，
dm8 上匿名 count 返回字符串 —— 已修复并全量回归（见下文 §6.1 末行）。

无已知副作用面被触碰：MRTR 封签/待处理表、只读门、标识符校验、通知 fan-out 均未改动语义。

### 独立回归（本报告 harness，relink 后二进制 sha256 `299a1049…`，ctest 3/3 + zorm ./run-test all 13/13）

| 回归 | 结果 |
| --- | --- |
| runner1 协议面 | 35/35 ✓ |
| runner2 数据工具（F-1/F-2 断言翻转为正向） | 97/97 ✓ |
| runner3 MRTR 对抗矩阵 | 40/40 ✓ |
| runner4 --allow-sql/通知/cwd/配置误用/CLI | 37/37 ✓ |
| runner5 HTTP 传输 | 17/17 ✓ |
| runner6 远端矩阵（F-8 断言翻转为正向） | 46/46 ✓ |
| runner7 HTTP MRTR principal 绑定（F-6 断言翻转） | 6/6 ✓ |
| **fix-verify（run8）：六项修复的正向验证** | **20/20 ✓** |
| **gate-sweep（run9）：13 条历史 bypass 仍拒 + 8 条合法语句仍放** | **25/25 ✓** |
| 真实客户端抽查（重建链接前，行为已生效）：F-1 拒绝/合法 where、F-2 PRAGMA 实数据、F-4 `count:*`→total、门拒 CTE-DML/EXPLAIN-CTA | 9/9 ✓ |

F-6 备忘：已注册的 MCP 客户端进程持有旧 exe 句柄时 `ninja` 链接报 `Permission denied`——重启客户端会话（杀掉 `zorm_mcp_server.exe` 子进程）即可重新链接；重建后的 `ctest` 与全部 harness 均在 HEAD 链接二进制上复跑通过。

**F-11 修复后全量回归**（`aggregateAliases` 对齐 `*`→`total` 默认）：runner1–7 + fix-verify + gate-sweep 共 **9 套 318 项断言 0 失败**，`ctest 3/3`。

## 7. 结论

- **13/13 工具、5/5 后端、双传输**的全部承诺行为在实地复现，未发现阻断/安全/一致性问题。
- MRTR 闸门在 stdio 与 HTTP、默认与 `--yes`、若干客户端形状与全部对抗路径下行为符合设计，这是本次测试验证面最大的部分（std io 40 + HTTP 补充）。
- 三个 P3 发现（F-1/F-2/F-9 之属）属于“参数卫生/错误文案可自纠性”打磨，也与本项目“报错即教学”的设计主张一致，值得尽快补齐；F-3 是 dm8 方言的固有语法约束，建议文档显式化。
- 复现入口：`.tmp-field/ft3/` 下 `run1_protocol.py … run7_http_mrtr.py`（`python runN.py > runN.out`），harness 及判定均自包含。

（测试执行与证据文件：`.tmp-field/ft3/run*.out`、`regress*.out`；修复回归 harness：`run8_fixverify.py`、`run9_gatesweep.py`。）
