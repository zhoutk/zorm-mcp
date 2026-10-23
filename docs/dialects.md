# 五后端方言差异与 zorm-mcp 的统一语义

本文记录 `sqlite3 / jsonfile / mysql / postgres / dm8` 在工具层仍可见的差异，以及服务端做的归一化。目标是：**同一套结构化工具语义在五个后端一致**；只有原始 SQL 逃生口保留方言本身的能力。

## 1. 标识符

| 项 | 行为 |
| --- | --- |
| 结构化工具接受的名字 | 简单标识符：字母/数字/下划线，不能以数字开头（`validIdentifier`） |
| 复杂名字（空格、连字符、引号、schema 前缀） | 用 `db_query_sql` / `db_exec_sql` 显式书写 |
| 校验时机 | 构造 SQL 之前；非法输入返回 `InvalidParams`，消息含 `invalid ... name` |
| 覆盖范围 | 表名、列名、`fields`、`group`、`sort` 列、`lks/ins/ors` 键、`sum/count` 列与别名、`data` 键、批量 `constraint`、`db_trans` 结构化元素的 `table`/`params` 键 |
| 原始 SQL | 不做标识符校验（显式逃生口） |

## 2. 占位符

| 后端 | 工具层统一写法 | 说明 |
| --- | --- | --- |
| sqlite3 / mysql / dm8 | `?` | 按 `values` 顺序绑定 |
| postgres | `?` 或 `$1..$n` | 服务端把字符串/注释/dollar-quote 之外的 `?` 顺序改写为 `$n`；`values` 个数必须与占位符一致 |

`db_query_sql` 与 `db_trans` 的文本元素使用同一规则；`db_exec_sql` 同样支持 `values`。

## 3. 分页

统一参数 `page`（1 起）/ `size`（1..500）：

- `size` 单独给：按 `page=1` 分页。
- `page` 单独给：明确报错，要求同时给 `size`（避免 zorm 生成 `limit 0,0` 的空结果）。
- 两者都给：正常分页；结果带 `records` / `pages`。

## 4. 元数据（db_describe_table）

| 后端 | 目录来源 | 注意事项 |
| --- | --- | --- |
| sqlite3 | `pragma_table_info` TVF | 主键列可能仍报 `nullable:true`（SQLite 元数据行为） |
| mysql | `information_schema.columns` | 驱动返回大写键（`IS_NULLABLE`/`COLUMN_DEFAULT`），已兼容 |
| postgres | `information_schema.columns` | `default` 可能显示 `NULL::character varying` |
| dm8 | `all_tab_columns`（按 schema） | `db_name` 同时作为 schema；类型大写 |
| jsonfile | 存储文件的行键 / columns 数组 | 类型为推断值（`string/number/...`） |

## 5. dm8 的 schema 语义

zorm 的 dm8 方言用 `db_name` 作为 **schema** 限定名（`"dbtest"."table"`）。因此：

- `db_list_tables` / `db_describe_table` / `db_indexes` 查询 `all_*` 目录并过滤 `owner = db_name`，与工具实际访问的对象一致；
- 如果登录用户的默认 schema 与 `db_name` 不同（例如 SYSDBA），不会再把“另一个 schema 的表”列出来；
- `db_delete where:{}` 使用同样的限定名。
- **raw DDL（`db_exec_sql`/`db_trans` 文本元素）必须自己写限定名并给标识符加引号**：`create table ft3_x (...)` 不会报错，但会落在登录用户的 schema（如 `SYSDBA`）且标识符折叠为大写，工具面按 `"dbtest"` 查询时**看不到**它。正确写法：`create table "dbtest"."ft3_x" ("id" varchar(32) primary key, ...)`（列名同样带引号小写）。结构化工具（`db_insert` 等）由 zorm 生成 quoted-lowercase SQL，不受影响。

## 6. 结果信封归一化

- 写操作同时提供 `affected` 与 `affectedRows`（源字段缺失时互补）。
- `db_count` 与 `db_query` 的 `sum/count` 别名值统一为 JSON 数字（dm8 驱动返回字符串）。
- `count` 的匿名 `*` 形式（`count:"*"`、`count:["*"]`、`count:{"*":""}`）别名为 `total`，不会生成非法的 `count(*) as *`；显式别名（`"*,n"`、`{"*":"n"}`）保持不变。
- 多行 `db_insert_batch` 不再返回单个 `insertId`。
- 文件型后端（sqlite3/jsonfile）的错误附加 `[file: 绝对路径]`；服务型后端（mysql/pg/dm8）不再附加误导性的仓库根路径。
- dm8 的 GBK 错误文本在返回前转 UTF-8（已是 UTF-8 的保持不变）。

## 7. 已知限制

- `db_query` 的 pair 编码（`lks/ins/ors`）假设值本身不含逗号（包括 `">,35"` 这类比较前缀）；含逗号的值会在参数校验阶段被**明确拒绝并指向** `db_query_sql` 参数绑定，不再返回含糊的 `ors is wrong`。
- jsonfile 的 `lks` 是子串匹配；`%` 通配符会被剥离后近似处理。
- **jsonfile 的原始 SQL 只支持 `select … from <table>`**（条件走 `params`，见 `db_query` 的 `lks/ins/ors`）。带 WHERE / JOIN / 聚合 / ORDER BY / LIMIT 的语句、以及不存在的表，一律返回**明确错误**（2026-10-22 实地报告 F4）；旧版返回 `status:202 data:[]` 会让模型误判为"没有这条数据"。
- `db_query_sql` 的 `EXPLAIN` 已在 mysql 适配（先执行再取结果元数据）；其它方言按各自支持面。`WITH`/`EXPLAIN` 的语句体会检查 DML 关键字，数据修改型 CTE 被拒绝；`EXPLAIN` 只能包裹 SELECT/WITH/VALUES，`EXPLAIN ANALYZE CREATE TABLE AS`、`EXECUTE`、`CALL`、`DECLARE` 与 DML 一律拒绝（MySQL 的只读 `EXPLAIN FOR CONNECTION` 放行）。`PRAGMA` 只放行已知只读名单（`table_info`/`index_list`/`foreign_key_list`/`integrity_check` 等带参形式与 `user_version`/`journal_mode`/`page_count` 等状态读取），`=` 与 `PRAGMA name(value)` 赋值形态一律拒绝。
- dm8 取数循环只把 `DSQL_SUCCEEDED` 当作"还有一行"，取数错误立即返回（不再无限 push 空对象）；单次查询上限 100000 行，超出返回明确错误。
- `db_query_sql` 的只读闸门是**语句级**的（白名单 + 语句体 DML 检测）。它不建立引擎级只读会话——`db_exec_sql` 与结构化写工具共用同一连接池，连接级只读会连带禁止写工具。需要物理隔离时，把只读探索指向只读账号的实例。
