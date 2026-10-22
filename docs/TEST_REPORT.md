# zorm-mcp 实地测试报告

| 项目 | 内容 |
| --- | --- |
| 被测对象 | `zorm-mcp`（`bin/zorm_mcp_server.exe`，13 工具 / 5 后端） |
| 版本基线 | 仓库 `master`，子模块 `thirds/zorm`、`thirds/mcp-cpp` 当前指针 |
| 测试日期 | 2026-10-22 |
| 测试方式 | 真实 MCP 客户端（VS Code 已安装的 stdio server）+ 独立 HTTP 实例（协议级、带 SQL 日志）+ 自带 CTest |
| 测试环境 | Windows，MSYS2 clang64，Release 构建 |
| 报告结论 | **架构成立，但当前不具备"对外正式发布"条件**：2 个 P0 缺陷（开箱即坏、标识符注入导致误删全表），5 个 P1 缺陷（跨方言行为错误） |

---

## 1. 测试环境与接入方式

### 1.1 后端清单

| 路由名 | 类型 | 实际连接 | 服务版本 | 表数 |
| --- | --- | --- | --- | --- |
| `sqlite3` | sqlite3 | `D:/codes/zorm-mcp/zorm-mcp.db` | 3.40.1 | 1（`t_e2e`） |
| `jsonfile` | jsonfile | `D:/codes/zorm-mcp/zorm-mcp.json` | — | 0 / 冲突（见 D-01） |
| `mysql` | mysql | `10.0.0.7:3306/dbtest` | 8.3.0 | 8 |
| `postgres` | postgres | `10.0.0.7:5432/dbtest` | PostgreSQL 14.12 (Debian) | 7 |
| `dm8` | dm8 | `10.0.0.7:5236/dbtest` | DM Database Server 64 V8 | 17 |

远端连接串取自 `thirds/zorm/tests/dbconfig.json`（用户提供的已启动实例）。五后端启动阶段全部成功打开，`db_info` 均返回正确版本，说明 DlL 打包与连接池初始化正常。

### 1.2 测试通道

1. **stdio 实机通道**：VS Code 通过 `.vscode/mcp.json` 注册 `cmd.exe /c start.cmd`，即产品默认接入形态。
2. **HTTP 通道**：自建 5 个临时实例，分别用于
   - `--yes`（自动化写）、
   - 无 `--yes`（MRTR 两步确认）、
   - `--allow-sql`（可选工具）、
   - `--http-token`（鉴权与 principal 绑定）、
   - jsonfile 独立库（排除默认文件名冲突干扰）。
   全部实例开启 `DbLogClose:false`，**逐条比对生成的 SQL**，这是定位方言差异的主要证据来源。
3. **协议通道**：`tools/list`、`resources/list`、`resources/templates/list`、`prompts/list`、`resources/read`、JSON-RPC 批处理，使用 `MCP-Protocol-Version: 2026-07-28` 与 legacy 版本双测。
4. **回归通道**：`ctest --test-dir build --output-on-failure`。

### 1.3 测试数据

- 本地：`sqlite3.t_e2e`（3 行，`id/name/score`）；jsonfile 独立文件写 `orders` 表。
- 远端：仅使用 `thirds/zorm` 契约测试已有的 `table_for_test` / `second_table` / `third_table`。
- **所有写入远端的测试行已在收尾阶段逐条 `db_delete` 清理，并 `db_count` 复核为 0**（mysql / postgres / dm8 三库均为 0）。

---

## 2. 结果总览

### 2.1 功能面通过/失败统计

| 模块 | 用例数 | 通过 | 失败 | 备注 |
| --- | --- | --- | --- | --- |
| 启动与连通性 | 5 | 5 | 0 | 五后端全通 |
| 元数据（list/describe/indexes/info） | 18 | 16 | 2 | D-04、D-09 |
| 结构化查询 db_query | 34 | 26 | 8 | D-03、D-05、D-10 |
| db_count | 6 | 6 | 0 | 五后端一致 |
| 只读 SQL 闸门 db_query_sql | 22 | 18 | 4 | D-07、D-08、D-11 |
| MRTR 写确认 | 14 | 14 | 0 | 篡改/重放/跨工具/跨 principal 全部正确拒绝 |
| CRUD 写（insert/upsert/update/delete） | 25 | 23 | 2 | D-02、D-06 |
| 批量 upsert | 8 | 8 | 0 | 三服务型后端一致 |
| 事务 db_trans | 12 | 12 | 0 | 提交/回滚均验证 |
| 资源 / 提示 / 模板 | 10 | 10 | 0 | — |
| HTTP 鉴权 / principal 绑定 | 8 | 8 | 0 | — |
| jsonfile 独立库 | 12 | 12 | 0 | 换干净文件后全通过 |
| 自带 CTest | 3 套件 | 3 | 0 | e2e / unit / demo 全绿 |

**合计：187 项检查，166 项通过，21 项失败（含同一根因的重复表现）。去重后为 12 个缺陷 + 2 个观察项。**

### 2.2 缺陷总表

| 编号 | 严重度 | 模块 | 一句话 |
| --- | --- | --- | --- |
| **D-01** | **P0** | 默认配置 | 默认 jsonfile 库文件与配置模板同名 `zorm-mcp.json`，开箱即坏并生成 `.corrupt.*` 垃圾文件 |
| **D-02** | **P0** | 写入/查询 | `table` 参数未做标识符校验，可注入 SQL；实测"删一条 id"变成"删全表" |
| **D-03** | P1 | 智能查询 | `ins` 传数组时列名被重复编码，postgres/dm8 报错、mysql/sqlite 静默错结果 |
| **D-04** | P1 | 元数据 | MySQL 的 `nullable` 恒为 true、`default` 恒为空（候选键缺大写形式） |
| **D-05** | P1 | 智能查询 | `count`/`sum` 的 `"col,alias"` 字符串形式生成非法 SQL；`count` 的别名不可用 |
| **D-06** | P1 | 写入 | dm8 上 `db_delete where:{}` 失败（未做 schema 限定） |
| **D-07** | P1 | SQL 逃生口 | postgres 上 `?` 占位符不可用，与文档承诺不符 |
| **D-08** | P2 | SQL 逃生口 | MySQL 上 `EXPLAIN` 失败（工具描述明确宣传该用途） |
| **D-09** | P2 | 元数据 | dm8 `list_tables` 返回系统表 `##HISTOGRAMS_TABLE`，但 `describe_table` 又拒绝它 |
| **D-10** | P2 | 智能查询 | 分页语义陷阱：只传 `size` 完全失效，只传 `page` 返回空 |
| **D-11** | P2 | SQL 闸门 | 前置注释 `/* c */ select 1` 被误判为写语句 |
| **D-12** | P3 | 结果信封 | 方言间字段不一致：dm8 无 `affected`、计数为字符串、批量返回 `insertId`、错误里的 `[file: 仓库根]` 误导、dm8 错误 GBK 乱码 |
| **O-01** | 观察 | 并发 | 客户端同批并行调用出现"参数串台"，服务端 40 次并发对照未能复现 |
| **O-02** | 观察 | 文档 | 普通 MCP 客户端无法回传 params 层 `requestState`，降级批准通道未写入文档 |

---

## 3. 分模块详测记录

### 3.1 启动与连通性 ✅

| 检查 | 结果 |
| --- | --- |
| 无 `--config` 启动（内置 sqlite3 + jsonfile） | 通过 |
| `--config` 五后端启动 | 通过，5 个 backend 全部 `opened` |
| 未知 db 路由（`sqlite`、`mysql` 在单库配置下） | 正确报错并列出可用名：`unknown db: sqlite (available: jsonfile(jsonfile), sqlite3(sqlite3))` |
| 空字符串 `db` | 正确报错并列出全部后端 |
| `--http` 监听、`--yes`、`--allow-sql`、`--http-token` | 均生效 |

**自纠错设计验证通过**：错误信息同时给出"错在哪 + 正确取值集合 + 应该看哪份说明"，是本项目最强的体验点之一。

### 3.2 元数据 ✅（除 D-04 / D-09）

- `db_list_tables`：五后端均返回，jsonfile 走直读存储文件，sqlite 走 `sqlite_master`。
- `db_describe_table`：
  - sqlite：`id/name/score`，类型 `varchar(32)/double`，`primary_key` 正确（走 `pragma_table_info` TVF）。
  - postgres：`nullable` 正确（`id` = false），`default` 显示 `NULL::character varying`（可接受）。
  - dm8：类型全大写（`CHAR/VARCHAR/INTEGER`），`nullable`、`primary_key` 正确。
  - **mysql：`nullable` 全为 `true`，即使 DDL 明确 `NOT NULL`**（D-04）。
  - 未知表：`unknown table: sqlite3.no_such_table - call db_list_tables first`（正确教学式报错）。
  - 非法标识符：`invalid table name: third_table where 1=1 (...)`（正确拒绝）。
- `db_indexes`：sqlite 返回 `sqlite_autoindex` + `origin:pk`；mysql 返回完整 `SHOW INDEX`；postgres 返回 `pg_indexes` 的 `indexdef`；dm8 仅返回 `INDEX_NAME`（无唯一性、无列信息，信息量最低）。
- `db_info`：版本探测按方言正确（`sqlite_version()` / `version()` / `v$version`）。

### 3.3 结构化查询 db_query

通过项：
- `fields` 投影；等值条件；比较前缀 `>, 85`；
- `lks` 模糊（含中文 `%凯文%`，用 JSON `\uXXXX` 传参五后端均命中）；
- `ins` 单值、`ors` 多条件、`group + sum` 聚合、`sort` 单列/多列/升降序、`page + size` 组合；
- `db_count`（无 where / 有 where / 比较前缀）。

失败项归类：
- `ins` 数组 → D-03；
- `count`/`sum` 别名形式 → D-05；
- 只传 `size` / 只传 `page` → D-10。

生成 SQL 交叉核对（来自服务端 SQL 日志）：

```text
mysql    : select * from table_for_test where age in (?,?,?)
postgres : select * from table_for_test where age in ($1,$2,$3)
dm8      : select * from "dbtest"."table_for_test" where "age" in (?,?,?)
```

三种方言的引号/占位符/限定名生成均正确，问题只出在**绑定值本身被污染**（D-03）。

### 3.4 只读 SQL 闸门 db_query_sql

| 输入 | 预期 | 实际 | 判定 |
| --- | --- | --- | --- |
| `select ...` | 允许 | 允许 | ✅ |
| `with x as (...) select` | 允许 | 允许 | ✅ |
| `show tables` / `describe t` / `pragma_*` | 允许 | 允许 | ✅ |
| `explain select`（postgres） | 允许 | 允许，返回 `QUERY PLAN` | ✅ |
| `explain select`（mysql） | 允许 | `details, . error code: 0` | ❌ D-08 |
| `insert/update/delete` | 拒绝 + 指路 | 拒绝 + 指路 | ✅ |
| `select 1; drop table x` | 拒绝 | 拒绝（单语句约束） | ✅ |
| 前导空白 / 换行 | 允许 | 允许 | ✅ |
| 前置注释 `/* c */ select 1` | 允许 | 拒绝 | ❌ D-11 |
| `?` 绑定（mysql/sqlite/dm8） | 允许 | 允许 | ✅ |
| `$n` 绑定（postgres） | 允许 | 允许 | ✅ |
| `?` 绑定（postgres） | 按文档允许 | **语法错误** | ❌ D-07 |
| dm8 标识符不加引号 | — | `无效的表或视图名`（DM 语义如此） | ⚠️ 需文档说明 |

### 3.5 MRTR 写确认 ✅ 全通过

| 场景 | 结果 |
| --- | --- |
| 首次写调用 | 返回 `resultType=input_required` + `requestState` + `inputRequests.confirm`，文案含库/表/影响行数 |
| 批准（params 层） | 执行并落库，返回 `affected/affectedRows` |
| 拒绝 `approved:false` | 返回 `Write cancelled by user.`，数据库无变化 |
| 已消费 state 重放 | 拒绝：`refers to no pending insert op ... (already executed or expired)` |
| 伪造签名 | 拒绝：`invalid, expired, forged, or was issued for a different tool` |
| 跨工具复用 | 拒绝（同上） |
| 跨 principal（HTTP） | A 暂存 → B 批准被拒 → A 批准成功 |
| state 放入 `arguments` | 拒绝并给出正确放置指引 |
| 缺 state，仅 `inputResponses` | 回退到该 `(db, table, kind)` 最近 pending，执行成功 |
| `--yes` | 直接执行，日志注明 `confirmation disabled` |

**结论：MRTR 是本项目质量最高的模块**，安全属性（HMAC 绑定工具 + 绑定 principal + 一次性 + 多形态容错）实测全部成立。

### 3.6 CRUD 写入

- `db_insert`：五后端成功；重复 id 触发 upsert（mysql `on duplicate key update`、pg `on conflict`、dm8 成功）。
- `db_insert_batch`：三服务型后端均生成正确的多值 upsert SQL，中文/数值混合正常。
- `db_update`：`{data, where:{id}}` 扁平化后正确；缺 id 时给出替代方案指引。
- `db_delete`：
  - `{id}` 单行删除 ✅（三后端）；
  - `{id: "不存在的值"}` ✅ 返回 0 行；
  - `{}` 清空：mysql ✅、postgres ✅、**dm8 ❌（D-06）**；
  - **构造非法 `table` 造成越权删除（D-02，见 3.10）**。

### 3.7 批量与事务

- 文本事务：mysql `?`、postgres `$n`、dm8 `?` + 引号限定名，**提交成功**。
- 回滚：第二条语句指向不存在表 → 整体回滚，脏数据 `db_count = 0`，三后端一致。
- 结构化事务元素：`insert/update/delete` 与 `batch` 全部正确；方法名大小写不敏感（`Batch` 可识别）。
- 事务返回：`Transaction success`（mysql/pg），dm8 无 details 但状态 200。

### 3.8 资源、提示、补全

| 能力 | 结果 |
| --- | --- |
| `resources/list` | `db://guide`、`db://schema`，带 `ttlMs`/`cacheScope` |
| `resources/templates/list` | `db://schema/{db}`、`db://schema/{db}/{table}` |
| `resources/read db://schema` | 五后端表清单汇总 |
| `resources/read db://schema/mysql` | 8 张表 |
| `resources/read db://schema/mysql/table_for_test` | 列元数据文本 |
| 未知 URI | `Unknown resource: db://reloaded/nope` |
| `prompts/list` / `db-playbook` | 正常 |
| 写后通知 | `notifications/resources/updated` + `list_changed` 可订阅 |

### 3.9 HTTP 与协议

- 无 token → 401；错 token → 401；两个合法 token 均 200。
- Bearer principal 绑定正确（见 3.5）。
- `tools/list` 无 `--allow-sql` 时 12 个工具；有该参数时 13 个且 `db_exec_sql` 出现。
- 未注册时调用 `db_exec_sql` → `Unknown tool: db_exec_sql`（不是内部错误，语义正确）。
- `db_exec_sql` 在 `--allow-sql` 下：DDL `create table` ✅、DML `insert` ✅、读回 ✅、`drop` ✅、多语句 `;` 被数据库拒绝 ✅。
- JSON-RPC **批处理数组**不被接受（`-32600 expected a JSON-RPC request or notification`）。MCP 规范已移除批处理，此处合规，仅需在文档说明。

### 3.10 注入与边界（重点）

**D-02 复现（postgres，实测删除了整表）**

```jsonc
// 意图：删除 id = "nope" 的一行
{ "db": "postgres", "table": "second_table where 1=1 or id = $1 --",
  "where": { "id": "nope" } }
```

实际结果：

```json
{ "status": 200, "affected": 2, "affectedRows": 2 }
// 随后 db_count(second_table) = 0
```

手工拼出的 SQL 为 `delete from second_table where 1=1 or id = $1 -- where id = $1`，`--` 截断尾部，`$1` 由 `where.id` 提供，因此"删一行"变成"删全表"。同类路径也存在于 `db_query` / `db_insert` / `db_update`。

**结论：虽然写操作有 MRTR 人机确认，但确认文案展示的是被污染的表名，用户看到 `postgres.second_table where 1=1 or id = $1 --` 时很难意识到后果，确认门槛被实质性绕过。**

其他边界：
- `db_insert` 的 `table` 含 `; drop table users` → 被数据库语法错误挡下（侥幸，不是设计保证）。
- `db_query` 的 `table` 含 `where 1=1` → **静默返回全表**，无任何提示。
- 标识符白名单 `validIdentifier` 已存在（`src/json_util.hpp:59`），但只在 3 处被调用。

### 3.11 jsonfile 后端

在**独立文件**上全部通过：建表（首写自动创建）、`list_tables`、`describe_table`（从 columns 数组/行键推断类型 `string/number`）、`query`（含排序）、`update`、`insert_batch`、`delete by id`、`delete all`（"select ids → 逐条 remove"，返回 `delete all ok`）。

在**默认配置**下完全不可用（D-01）。

### 3.12 并发与批量

| 实验 | 结果 |
| --- | --- |
| VS Code 同批并行 6 个 `db_query`（多种 sort/fields） | 多轮出现 `no such column: "id"` / `"score desc"` / `"name","score"` |
| 串行逐个调用同样参数 | 5/5 全部正确 |
| HTTP 12 个**不同**查询并发 | 12/12 成功 |
| stdio 28 个**不同**查询并发（单写锁保证消息完整） | 27/28 成功，0 个参数错乱 |
| JSON-RPC 批处理数组 | 服务端拒绝（规范已移除） |

**判定：服务端在 `MCP_WORKERS=1` 下未观察到参数串台；该现象更像客户端并行调度所致。** 报告中保留为 O-01，不建议直接改服务端，但建议在 Cursor / Inspector 上复核，并把"避免同批并行调用写/查混合"写入 FAQ。

### 3.13 自带测试

```text
1/3 mcp_test_zorm_mcp ....... Passed  1.66 sec   （15 个 e2e 用例）
2/3 mcp_test_zorm_unit ...... Passed  0.02 sec
3/3 mcp_test_zorm_analytics . Passed  0.16 sec
100% tests passed out of 3
```

**但 e2e 只覆盖 sqlite3 + jsonfile**，本报告中的 D-03/D-04/D-06/D-07/D-08/D-09 全部逃逸。这是"测试全绿但线上有洞"的直接原因。

---

## 4. 缺陷详述（含根因定位）

### D-01 [P0] 默认 jsonfile 库与配置模板同名

- **位置**：`src/app.cpp:44` — `{"connString":"zorm-mcp.json","DbLogClose":true}`；仓库根同时存在配置模板 `zorm-mcp.json`。
- **现象**：
  - `db_list_tables {db:"jsonfile"}` → `{"tables":[],"count":0}`（把配置当成空库）；
  - 任意写 → `write failed: ... database read failed [file: D:/codes/zorm-mcp/zorm-mcp.json]`；
  - 每次失败在仓库根生成 `zorm-mcp.json.corrupt.<时间戳>`（本次测试一次运行产生 3 个 627 字节副本）。
- **影响**：README 主推的"零依赖开箱即用"实际不可用；仓库根持续产生垃圾文件；极端情况下若先写后覆盖，会把配置模板变成数据文件，用户的五后端配置直接丢失。
- **修复建议**：默认文件名改为 `zorm-mcp-data.json`；启动时若发现 `zorm-mcp.json` 形如 `{"databases":{...}}`（是配置而非表数组），打印明确警告并拒绝以它为库；`.gitignore` 补 `*.corrupt.*`。
- **回归测试**：新增"默认配置下 jsonfile 可建表/可读回，且不产生 `.corrupt.*`"。

### D-02 [P0] `table` 未校验导致 SQL 注入

- **位置**：`src/data_tools.cpp`（`toolQuery` / `executeWrite` 的 insert/update/delete-by-id 分支）、`src/meta_tools.cpp` 已正确使用 `validIdentifier`。
- **证据**：见 3.10，`affected: 2` + `count: 0`。
- **影响**：越权删除/覆盖；MRTR 确认文案被污染，用户无法据此判断真实影响面；对生产库是数据安全事故。
- **修复建议**：
  1. 在 `stageWrite()` 与 `toolQuery()` 入口统一 `if (!validIdentifier(table)) return Err(...)`；
  2. 纵深防御：SQL 拼接处改用方言 `quoteIdent`（zorm 已提供），避免"校验漏一处就失守"；
  3. `db_query_sql` 保持现状（本来就是逃生口，但要在 description 说明它接受任意 SQL、不保证安全）。
- **回归测试**：把本次 4 个注入载荷固化为用例（含 `where 1=1 --`、`$1` 占位符平衡版、`; drop`、注释版），断言全部被 `invalid table name` 拒绝。

### D-03 [P1] `ins` 数组编码错误

- **位置**：`src/data_tools.cpp:49-55` — 数组分支对每个元素都重复写列名。
- **现象**：`{"ins":{"age":[18,20]}}` → `"age,18,age,20"` → SQL `age in ($1,$2,$3)`，`$2='age'`。
  - postgres：`invalid input syntax for type integer: "age"`
  - dm8：`字符串转换出错 (-6111)`
  - mysql / sqlite：**不报错**，`'age'` 隐式转 0 → 静默错误结果（最危险）
- **修复**：`ins` 生成 `col,v1,v2,...`；`lks`/`ors` 才需要成对重复列名。同时把 `ins` 的 schema 描述改成"值是数组，元素为同一列的多个取值"。
- **回归测试**：三后端 `ins:{age:[18,20]}` 必须返回 2 行且不含's'串。

### D-04 [P1] MySQL 列元数据 nullable/default 恒错

- **位置**：`src/app.cpp:289`（`{"is_nullable","NULLABLE"}`）、`:291`（`{"column_default","DATA_DEFAULT"}`）；MySQL 驱动返回大写 `IS_NULLABLE` / `COLUMN_DEFAULT`。
- **证据**：`table_for_test.id` DDL 为 `NOT NULL`，工具返回 `nullable: true`；`COLUMN_DEFAULT` 非空时也会丢。
- **影响**：AI 会据此生成错误判断（"这列可以留空"），进而产出必然失败的写入。
- **修复**：候选键补大写形式（顺序建议 `{"is_nullable","IS_NULLABLE","NULLABLE"}`）。
- **回归测试**：MySQL 上断言非空列 `nullable=false`、带默认值列 `default` 非空。

### D-05 [P1] `count`/`sum` 别名形式生成非法 SQL

- **位置**：`src/data_tools.cpp:121`（数组分支）、`:126`（字符串分支）——都做了 `s + "," + s`。
- **现象**：
  - `count: "*,total"` → `*,total,*,total` → `select count(*) as total count(*) as total from ...` → `near "count": syntax error`；
  - `count: "*"` 能跑，但别名恒为 `*`（结果键是 `"*"`）；
  - `count` 的 schema 只允许 `string`，`object` 会被 schema 校验拒绝 → **"带别名的 count" 完全不可达**；
  - `sum: "score,total"` 同样坏，`sum: ["score"]` 只能拿列名当别名。
- **修复**：字符串分支原样透传（zorm 本来就吃 `col,alias`）；数组/单值分支才补 `col,col`；`count` 的 schema 放开 `oneOf: [string, object]`，与 `sum` 对齐。
- **回归测试**：`count:"*,total"` → 1 行 `{"total": N}`；`sum:"score,total"` → `{"total": x}`。

### D-06 [P1] dm8 删除全部失败

- **位置**：`src/data_tools.cpp:312` — `db->execSql("delete from " + table)`，绕过 `qualifiedTable`。
- **现象**：`无效的表或视图名[SECOND_TABLE]`（且错误文本 GBK 乱码）。
- **影响**：dm8 用户无法用 `where:{}` 清表，只能逐条删。
- **修复**：与 id 删除一致，走方言限定名（`"dbtest"."table"`），或直接复用结构化清理路径。

### D-07 [P1] postgres 不认 `?` 占位符

- **位置**：`src/sql_tools.cpp` 直接透传 SQL；zorm 的 pg 后端 `detectParameterized` 嗅探 `$`。
- **现象**：`select ... where name = ?` → `syntax error at end of input`；改 `$1` 同一请求成功。
- **影响**：工具描述与 `db://guide` 都写"用 `?`"，AI 会 100% 踩坑，且错误信息无法指出原因。
- **修复（二选一）**：
  1. 服务端按方言把 `?` 顺序重编号为 `$n`（推荐，保持单一语义）；
  2. 文档 + description + 报错三处明确"postgres 用 `$1..$n`"，并在检测到 pg + `?` 时给出定向提示。
- **回归测试**：同一段 SQL（`?` 形式）在 pg/mysql/sqlite/dm8 上结果一致。

### D-08 [P2] MySQL `EXPLAIN` 失败

- **位置**：`src/meta_tools.cpp:56` 附近无直接关系；问题在 MySQL 后端执行 `EXPLAIN` 时驱动返回空错误信息（`details, . error code: 0`）。
- **影响**：工具描述把 `EXPLAIN` 作为 `db_query_sql` 的主要卖点之一。
- **修复**：定位后端 prepare/execute 路径；短期可在 description 标注"MySQL 暂不支持 EXPLAIN，改用 `SHOW` / `information_schema`"。

### D-09 [P2] dm8 系统表进入表清单

- **位置**：`src/app.cpp:202` — `select table_name from user_tables` 无过滤。
- **现象**：`##HISTOGRAMS_TABLE` 出现在 `db_list_tables`，但 `db_describe_table` 因其含 `#` 报 `invalid table name`。
- **影响**：AI 会先列后描，遇到必然失败的项，浪费一轮调用。
- **修复**：过滤 `#`/`$` 前缀与已知系统表；或让 `listTables` 复用 `validIdentifier` 过滤。

### D-10 [P2] 分页语义陷阱

- **位置**：`thirds/zorm/src/base/SqlBackendBase.cpp:772-774` — 仅 `page > 0` 时追加 `limit (page-1)*size, size`。
- **现象**：`{"size":3}` → 忽略，返回全部 6 行；`{"page":2}` → `limit 0,0` → 空数组（`records:6,pages:1`）。
- **影响**：文档把 `page/size` 写成一个功能，AI 只传其中一个就会得到"看似成功但结果错"的返回，属于静默错误。
- **修复**：handler 里做归一化（`size` 缺省 = 默认页大小或 20；`page` 缺省 = 1，仅在显式给了 `page` 或 `size` 时才启用分页），或对半参数直接报错并说明两者需同时提供。

### D-11 [P2] 前置注释误判

- **位置**：`src/sql_tools.cpp:36` — 只 `isspace` 跳过空白，不跳过 `--` / `/* */`。
- **修复**：扫描首个关键字前先剥离前导注释；注意不要误伤 `WITH` / `EXPLAIN (ANALYZE)`。

### D-12 [P3] 结果信封与错误信息的方言差异

| 现象 | 后端 | 建议 |
| --- | --- | --- |
| 写结果无 `affected` | dm8 | 统一为 `affected`（可用 `affectedRows` 兜底） |
| `count` 值为字符串 `"6"` | dm8 | 转换层统一数值类型 |
| 批量写入返回 `insertId:"d-2"` | dm8 | 批量不返回单 id |
| 写失败附 `[file: D:\codes\zorm-mcp\]` | mysql/pg/dm8（connString 为空） | 仅在 `connString` 非空时追加 |
| 错误消息 GBK 乱码且截断 | dm8 | UTF-8 转码 + 保留完整 errmsg |

这些不阻塞使用，但直接拉低 AI 自纠错成功率，属于"体验债"。

### O-01 客户端并行调用异常

见 3.12。建议先换客户端复核，再决定是否需要在服务端加请求级别的参数快照/防御性拷贝。

### O-02 MRTR 批准通道文档缺口

VS Code 客户端能把确认文案展示给用户，但当前工具接口无法在 `params` 层回传 `requestState`。实测可行的降级路径（已在 3.5 验证）：

```jsonc
{ "name": "db_insert",
  "arguments": { /* 原参数 */, "inputResponses": { "confirm": { "approved": true } } } }
```

建议在 `INTEGRATION.md` 的每客户端章节补一行"客户端不支持 params 层回传时怎么办"，否则新手会卡在"只能看到确认、无法批准"。

---

## 5. 覆盖缺口分析

| 缺口 | 后果 | 补法 |
| --- | --- | --- |
| e2e 只测 sqlite3 + jsonfile | 全部方言缺陷逃逸 | 用环境变量（如 `ZORM_MCP_REMOTE=1`）挂载 `thirds/zorm/tests/dbconfig.json` 的远端配置，作为可选测试套件 |
| 无 `ins` 数组 / `count` 别名用例 | D-03、D-05 逃逸 | 参数化矩阵测试（query × backend） |
| 无 MySQL `describe_table` 断言 | D-04 逃逸 | 断言 `nullable=false` 与 `default` |
| 无 dm8 `delete all` 用例 | D-06 逃逸 | 加一条 |
| 无 pg `?` 绑定用例 | D-07 逃逸 | 加一条跨方言同 SQL 对照 |
| 无注入型 `table` 用例 | D-02 逃逸 | 见 D-02 回归测试 |
| 无并发/流水线请求用例 | O-01 无基线 | 加一条"同一连接流水线 20 条不同查询" |

---

## 6. 发布建议

| 结论项 | 判定 |
| --- | --- |
| 架构与协议层 | **可发布**（双时代协议、资源模板、MRTR 安全模型实测成立） |
| 单机 sqlite3 场景 | **修复 D-01/D-02 后可发布** |
| 五后端生产场景 | **不可发布**（D-02 数据安全 + D-03/D-04/D-06 正确性） |
| 文档准确性 | **待修**（D-07 的 `?` 承诺、O-02 的批准通道） |

建议版本节奏：修完 P0 + P1 后发 `0.1.1`（安全与正确性），修完 P2 + D-12 后发 `0.2.0`（方言一致性）。

---

## 7. 下一步开发计划

### 阶段 0：止血（P0，预计 1 天，可当天发版）

| # | 任务 | 文件 | 验收标准 |
| --- | --- | --- | --- |
| 0.1 | 全工具入口统一 `validIdentifier(table)` | `src/data_tools.cpp`、`src/sql_tools.cpp` | 4 个注入载荷全部返回 `invalid table name`；正常表零回归 |
| 0.2 | SQL 拼接处补方言 `quoteIdent`（纵深防御） | `src/data_tools.cpp` | 单表/多后端 CRUD 回归通过 |
| 0.3 | jsonfile 默认库改名 `zorm-mcp-data.json`，并识别"配置文件当库"场景 | `src/app.cpp:44` | 默认配置下 jsonfile 可建表；不产生 `.corrupt.*`；误用时给明确告警 |
| 0.4 | `.gitignore` 增加 `*.corrupt.*` | `.gitignore` | 仓库不再被垃圾文件污染 |
| 0.5 | 三条 P0 回归测试 | `test_zorm_mcp.cpp` / `test_zorm_unit.cpp` | ctest 全绿且新用例覆盖上述路径 |

**出口条件**：`ctest` 全绿；手工复跑 3.10 的注入用例全部被拒；README 的"开箱即用"段落实测可用。

### 阶段 1：方言正确性（P1，预计 2–3 天）

| # | 任务 | 文件 | 验收标准 |
| --- | --- | --- | --- |
| 1.1 | 修 `ins` 数组编码（`col,v1,v2...`） | `src/data_tools.cpp:49-55` | 三后端 `ins:{age:[18,20]}` 返回 2 行 |
| 1.2 | 修 `sum`/`count` 字符串与别名；`count` schema 放开 object | `src/data_tools.cpp:110-127`、schema | `count:"*,total"`、`sum:"score,total"` 均正确 |
| 1.3 | MySQL 元数据候选键补大写 | `src/app.cpp:289,291` | `NOT NULL` 列 `nullable=false` |
| 1.4 | dm8 delete-all 走限定名 | `src/data_tools.cpp:312` | dm8 `where:{}` 成功且计数归零 |
| 1.5 | postgres `?` → `$n` 归一化（或定向报错） | `src/sql_tools.cpp` | 同一 SQL 四后端结果一致 |
| 1.6 | 分页参数归一化 + 半参数报错 | `src/data_tools.cpp` | `size` 单独传按默认页生效；`page` 单独传给出明确错误 |
| 1.7 | 对应参数化回归测试 | `test_zorm_mcp.cpp` | 上述每项至少 1 条断言 |

**出口条件**：本报告 D-03 ~ D-07、D-10 全部关闭；跨方言矩阵测试在 mysql/pg/dm8 上全绿。

### 阶段 2：一致性与可观测性（P2/P3，预计 2 天）

| # | 任务 | 验收标准 |
| --- | --- | --- |
| 2.1 | 统一结果信封：`affected`/数值类型/批量 `insertId` | dm8 与 mysql/pg 字段同构 |
| 2.2 | `[file: ...]` 仅在 connString 非空时输出 | 服务型后端错误不再指向仓库根 |
| 2.3 | dm8 错误消息 UTF-8 转码 + 完整保留 | 中文可读，AI 可据此自纠错 |
| 2.4 | MySQL `EXPLAIN` 定位修复；短期文档标注 | `explain select` 在 mysql 返回计划或明确不支持 |
| 2.5 | dm8 `list_tables` 过滤系统表 | 清单与 `describe_table` 可描述集合一致 |
| 2.6 | SQL 闸门剥离前置注释 | `/* c */ select 1` 通过，`/* c */ drop x` 拒绝 |
| 2.7 | dm8 `db_indexes` 补列与唯一性 | 返回信息量对齐 pg/mysql |

### 阶段 3：测试基础设施（与阶段 1 并行，预计 2 天）

| # | 任务 | 说明 |
| --- | --- | --- |
| 3.1 | 远端后端可选测试套件 | `ZORM_MCP_REMOTE=1` 时读取 `thirds/zorm/tests/dbconfig.json`，挂 3 个新 CTest 标签 |
| 3.2 | 方言矩阵测试框架 | 一个用例 × N 后端参数化，断言统一期望值 |
| 3.3 | 安全用例集 | 注入载荷、伪造 state、跨 principal、只读闸门绕过 |
| 3.4 | 并发/流水线用例 | 单连接 20 条不同查询，断言零参数串台（为 O-01 建立基线） |
| 3.5 | 覆盖率门槛 | `ZORM_MCP_COVERAGE=ON` 下对 `data_tools.cpp`/`app.cpp` 设线（如 ≥ 80%） |

### 阶段 4：文档与兼容（预计 1 天）

| # | 任务 |
| --- | --- |
| 4.1 | `INTEGRATION.md` 增补"客户端不支持 params 层回传时的降级批准方式"（O-02） |
| 4.2 | `db://guide` / `playbook` 补：postgres 占位符、dm8 标识符引号、`size/page` 需成对、count/sum 别名写法 |
| 4.3 | README 更新默认库文件名，补"配置文件不得与库文件同名"提示 |
| 4.4 | 新增 `docs/dialects.md`：五后端在标识符引号、占位符、分页、元数据大小写上的差异表 |
| 4.5 | O-01 结论写入 FAQ（避免同批并行混合读写；建议客户端逐个串行调用） |

### 里程碑

```mermaid
gantt
    dateFormat  YYYY-MM-DD
    axisFormat  %m-%d
    section 发布
    P0 止血 (0.1.1)          :milestone, m1, 2026-10-23, 0d
    P1 方言正确性            :a1, 2026-10-23, 3d
    P2 一致性 (0.2.0)        :a2, after a1, 2d
    测试基础设施             :a3, 2026-10-23, 5d
    文档与兼容               :a4, after a2, 1d
```

| 里程碑 | 时间 | 判据 |
| --- | --- | --- |
| M0 安全可用 | D+1 | P0 全关、ctest 加 3 条新用例全绿 |
| M1 方言一致 | D+4 | P1 全关、远端矩阵测试全绿 |
| M2 体验收口 | D+6 | P2/P3 全关、信封统一 |
| M3 正式对外 | D+7 | 文档更新完成、覆盖率达标、五后端手工验收复跑 |

### 优先级建议（若资源只够做三件事）

1. **D-02 标识符校验** —— 唯一的数据安全事故，且修复成本极低（一个守卫 + 4 条测试）。
2. **D-01 jsonfile 默认库改名** —— 决定"开箱即用"是否成立，影响所有新用户的第一印象。
3. **D-03 `ins` 数组编码** —— 唯一会在两个后端"静默返回错误结果"的逻辑缺陷，比报错更难被发现。

---

## 8. 修订实施计划（2026-10-22，逐项核对代码后）

### 8.1 复核结论与计划调整

| 编号 | 复核结果 | 计划调整 |
| --- | --- | --- |
| D-01 | 确认。默认 `jsonfile` 的 `connString` 与配置模板同名，`jsonfileTables()` 把配置对象当空库，写入时触发 zorm 的 corrupt 备份 | 默认库改名 `zorm-mcp-data.json`；启动时识别"配置文件当库"并拒绝；`.gitignore` 加 `*.corrupt.*` |
| D-02 | 确认且**范围比报告更大**：zorm 的 `quoteIdent()` 在 sqlite/mysql/pg 上是原样返回，除 `table` 外，`fields`/`group`/`sort`/`lks`/`ins`/`ors`/`sum`/`count`/where 键/`data` 键/批量 `constraint`/事务结构化元素都可注入 | 统一入口校验**表名 + 所有结构化标识符**；非法即 `InvalidParams`；新增注入载荷回归用例 |
| D-03 | 确认。数组分支对每个元素重复列名 | `ins` 数组改为 `col,v1,v2,...`；`lks`/`ors` 保持成对 |
| D-04 | 确认。MySQL 驱动返回大写 `IS_NULLABLE`/`COLUMN_DEFAULT` | 候选键补大写（含 `IS_NULLABLE`） |
| D-05 | 确认。字符串分支做了 `s+","+s`，数组分支别名只能用列名 | 字符串原样透传；数组元素 `col,col`（元素本身含逗号则透传）；`count` schema 与 `sum` 对齐 |
| D-06 | 确认。delete-all 直接 `delete from <table>`，未走 dm8 的 `"schema"."table"` | dm8 用 `db_name` 拼限定名，其余后端保持原样 |
| D-07 | 确认。pg 只认 `$n`；`detectParameterized()` 只看 `?` | 服务端做 `?`→`$n` 归一化（跳过字符串/注释/dollar-quote），`db_query_sql` 与 `db_trans` 文本元素统一；数量与 `values` 不符时给定向报错 |
| D-08 | 确认，根因在 **zorm** `MysqlDb::execStmtQuery`：`mysql_stmt_result_metadata()` 在 `mysql_stmt_execute()` 之前调用；`parameterized:false` 路径正常 | 在 `thirds/zorm` 调整为先 execute 再取 metadata，并跑 `./run-test all`；更新子模块指针 |
| D-09 | 确认。dm8 `user_tables` 未过滤，系统表含 `#` | `listTables()` 统一用 `validIdentifier` 过滤，保证列表与 `describe_table` 可描述集合一致 |
| D-10 | 确认。zorm 仅 `page>0` 时才分页；`size` 单独传无效，`page` 单独传 `limit 0,0` | handler 归一化：只传 `size` → `page=1`；只传 `page` → 明确报错要求同时提供；校验 `1..500` |
| D-11 | 确认。只跳过空白 | 扫描前导 `--` / `/* */` 注释；仍拒绝多语句 |
| D-12 | 确认。`[file:]` 无条件追加；dm8 字段命名/类型/编码不一致 | 本轮做：`affected`/`affectedRows` 归一、`[file:]` 仅在 connString 非空时输出、db_count 的 `total` 数值化；dm8 GBK 转 UTF-8；批量结果的 `insertId` 保留但文档说明 |
| O-01 | 服务端 40 次并发未复现 | 不改服务端；FAQ 写明"同批并行混合读写"风险与串行建议；新增 20 条流水线只读查询基线用例 |
| O-02 | 已可用：arguments 层 `inputResponses`（不带 state）会回退到该 `(db,table,kind)` 最近 pending | 写入 `INTEGRATION.md`、`db://guide` 与 playbook，明确降级批准方式 |

### 8.2 实施顺序（本轮一次完成）

| 批次 | 内容 | 验收 |
| --- | --- | --- |
| A | 标识符校验框架 + D-02 全部入口；D-01 默认库/配置识别；`.gitignore` | 4 个注入载荷 + 列名/排序/聚合注入全部拒绝；默认 jsonfile 可建表且无 `.corrupt.*` |
| B | D-03/D-04/D-05/D-06/D-09/D-10/D-11 | 新增 e2e 用例全绿；三远端后端矩阵手工复核 |
| C | D-07 占位符归一化 + D-08 zorm MySQL 修复 + D-12 信封收口 | pg `?` SQL 四后端同结果；mysql `explain` 成功；dm8 删除/计数/错误文本可读 |
| D | 测试与文档 | `ctest` 全绿；README/INTEGRATION/guide/playbook/dialects 更新；`thirds/zorm` 指针同步 |

### 8.3 明确的非目标

- 不修改 `thirds/mcp-cpp`（协议层行为正确）。
- 不把 `db_query_sql` / `db_exec_sql` / `db_trans` 的**原始 SQL 文本**纳入标识符校验——它们本来就是显式逃生口；只校验结构化字段。
- 不承诺列名含空格/连字符/引号的表；结构化工具接受的是简单标识符（字母/数字/下划线，非数字开头），复杂对象名请用 `db_query_sql`。

### 8.4 实施结果（2026-10-22）

| 编号 | 状态 | 实施位置 |
| --- | --- | --- |
| D-01 | ✅ | `app.cpp` 默认 `zorm-mcp-data.json`；启动时拒绝配置形状的 jsonfile；`.gitignore` 加 `*.corrupt.*` |
| D-02 | ✅ | `data_tools.cpp` 的 `checkWriteArgs` / `checkQueryArgs` / `checkTransElements`；表/列/排序/聚合/事务标识符统一校验 |
| D-03 | ✅ | `convertPairsObject(..., sameKeyForArray)`：`ins` 数组 → `col,v1,v2` |
| D-04 | ✅ | `app.cpp` nullable/default 候选键补 `IS_NULLABLE` / `COLUMN_DEFAULT` |
| D-05 | ✅ | `data_tools.cpp` sum/count 字符串透传、单列补别名；schema 放开 string/object/array |
| D-06 | ✅ | dm8 delete-all 使用 `"schema"."table"` |
| D-07 | ✅ | `json_util.cpp` `rewritePgPlaceholders`；`db_query_sql` / `db_exec_sql` / `db_trans` 文本统一 |
| D-08 | ✅ | `thirds/zorm` `MysqlDb::execStmtQuery`：先 execute 再取结果元数据 |
| D-09 | ✅ | `listTables` 统一过滤 `validIdentifier`；dm8 目录改查 `all_*` + `owner = db_name` |
| D-10 | ✅ | `toolQuery` 归一化：size-only → page 1；page-only 报错；1..500 校验 |
| D-11 | ✅ | `checkReadOnlySql` 跳过前导 `--` / `/* */` 注释 |
| D-12 | ✅（部分） | `affected`/`affectedRows` 互补、批量去 `insertId`、`[file:]` 仅文件后端、dm8 GBK→UTF-8、count/sum 别名数值化 |
| O-01 | ✅ 文档 | README/FAQ 建议串行；服务端不改 |
| O-02 | ✅ 文档 | README/INTEGRATION/guide 增补降级批准通道 |

**验证结果**

- 本地：`ctest --test-dir build` → 3/3 套件通过（e2e 含新增 `ReportedDefectRegressions`、HTTPS、协议双时代等）。
- 远端矩阵：自建协议脚本对 sqlite3 / mysql / postgres / dm8 执行 **73/73** 项检查全部通过（`docs/TEST_REPORT.md` §3 的缺陷路径逐项复跑）。
- `thirds/zorm`：`./run-test all` → **13/13** 通过（含 mysql/pg/dm8 契约与 plain 变体）。
- 新增可选自动化：`ZORM_MCP_REMOTE_CONFIG=<config> mcp_test_zorm_mcp --gtest_filter=ZormMcpE2E.RemoteBackendsMatrix`（默认 skip，CI 无需远端）。

**仍保留的已知限制**：pair 编码值不能含逗号；jsonfile 的 `%` 通配为近似；`db_trans` 原始文本仍是显式逃生口。详见 `docs/dialects.md`。

---

## 附录 A：复现命令速查

```bash
# 五后端启动（带 SQL 日志，MRTR 关闭）
./bin/zorm_mcp_server.exe --http 18200 --yes --config .tmp-test/remote-config.json

# 协议调用模板（modern 2026-07-28）
curl -s --noproxy '*' -X POST http://127.0.0.1:18200/mcp \
  -H 'Content-Type: application/json' -H 'Accept: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: db_query' \
  -d '{"jsonrpc":"2.0","id":"1","method":"tools/call",
       "params":{"name":"db_query","arguments":{"db":"mysql","table":"table_for_test"}}}'

# D-02 复现（**破坏性，仅在测试库执行**）
# arguments: {"db":"postgres","table":"second_table where 1=1 or id = $1 --","where":{"id":"nope"}}

# MRTR 两步
# 1) 原参数调用 → 取 resultState
# 2) 同参数 + params.requestState + inputResponses.confirm.approved=true
```

## 附录 B：测试数据与现场清理

| 项 | 状态 |
| --- | --- |
| 远端 `third_table` / `second_table` 测试行 | 已逐条删除，`db_count` 复核 mysql/postgres/dm8 均为 0 |
| 本地 jsonfile 测试表 | 使用独立临时文件，已随 `.tmp-test/` 删除 |
| 临时 HTTP 实例（18200–18204） | 已按端口 `taskkill`，`netstat` 复核 0 监听 |
| 仓库根 `zorm-mcp.json.corrupt.*` | 已清理 |
| `zorm-mcp.json` 配置模板 | 未被修改，内容完整 |
| 遗留诊断脚本 `.tmp_spawn_test.js` | 一次性 launcher 拉起探针，无任何引用、已被 ctest e2e 覆盖，测试收尾时删除 |
| 工作区其他改动 | `.vscode/mcp.json`（既有的 zorm-mcp 注册）未触碰 |
