# zorm-mcp 实地测试复现手册（照着做，一路可验证）

> 目标：**从空库开始**，在五种数据库后端中**随机抽一个**（想全跑也行），把
> [FIELD-TEST-REPORT-2026-10-23.md](FIELD-TEST-REPORT-2026-10-23.md) 里的测试完整过一遍；
> 每一步都给出**预期结果**，不符即停，最后一节给出"一键脚本复跑"。
> 全程约 30–60 分钟（远端三库 Need 10.0.0.7 可达；只测本机两个后端约 15 分钟）。

---

## 0. 准备（一次性）

1. **构建**（若 `bin/zorm_mcp_server.exe` 不存在或源码有改动）：

   ```bash
   git submodule update --init --recursive
   cmake --preset clang-rlz && cmake --build build -j 8
   ```

   验证 P0-1：`bin/zorm_mcp_server.exe` 已生成，且同目录有 40+ 个 DLL（bundled 闭包）。`cmake --build` 末行应含 `zorm: bundled N resolved runtime DLL(s)`。

2. **确认基线测试通过**（可选但建议，作为"代码没坏"的对照）：

   ```bash
   ctest --test-dir build                        # 预期: 100% tests passed out of 3
   cd thirds/zorm && ./run-test all              # 预期: 100% tests passed out of 13
   ```

3. **准备一个空库目录**（本手册所有文件都放这里，结束时删光即可）：

   ```powershell
   mkdir D:\zorm-test   # 任意位置，下文称 <T>
   ```

4. **写入空库配置**（五后端全配，后续随机抽用；把密码换成你的）：

   把下面内容存为 `<T>\cfg.json`。⚠️ 关键点：`connString` 用**绝对路径**，且**指向尚不存在的文件**——这就是"空库"。

   ```json
   { "databases": {
       "sqlite3":  { "type": "sqlite3",  "options": { "connString": "D:/zorm-test/probe.db" } },
       "jsonfile": { "type": "jsonfile", "options": { "connString": "D:/zorm-test/probe.json" } },
       "mysql":    { "type": "mysql",    "options": { "db_host": "10.0.0.7", "db_port": 3306, "db_name": "dbtest", "db_user": "root", "db_pass": "123456" } },
       "postgres": { "type": "postgres", "options": { "db_host": "10.0.0.7", "db_port": 5432, "db_name": "dbtest", "db_user": "root", "db_pass": "123456" } },
       "dm8":      { "type": "dm8",      "options": { "db_host": "10.0.0.7", "db_port": 5236, "db_name": "dbtest", "db_user": "SYSDBA", "db_pass": "123456" } }
   } }
   ```

   验证 P0-2：`<T>` 里此时**没有** `probe.db` / `probe.json`（空库状态，后面会分别验证两个文件后端的"懒创建"）。

5. **接入一个 AI 客户端**（推荐 Claude Code 或 VS Code/Copilot；完整清单见 [INTEGRATION.md](../INTEGRATION.md)）：

   ```bash
   claude mcp add zorm-mcp -- D:/codes/zorm-mcp/start.cmd --config D:/zorm-test/cfg.json
   ```

   重启客户端会话，验证 P0-3：工具列表里能看到 **12 个** `zorm-mcp-db_*` 工具（默认无 `db_exec_sql`，见 §7）。

6. **抽签选本轮的后端**（五种随机一个；想全跑就每个后端重复 §2–§5）：

   ```powershell
   Get-Random -InputObject sqlite3,jsonfile,mysql,postgres,dm8
   ```

   下文把选中的后端记作 **B**。远端后端（mysql/postgres/dm8）需要 10.0.0.7 可达；不可达时从 sqlite3/jsonfile 里抽。

---

## 1. 启动与协议面（对应报告 runner1）

对 B 调用（在客户端对话里说/做）：

1. `db_info {db: B}`
   - **预期**：`ready:true`，`server_version` 非空。各后端实测值：sqlite3 `3.40.1`；jsonfile 无版本字段（`ready:true` 即可）；mysql `8.3.0`；postgres `PostgreSQL 14.12…`；dm8 `DM Database Server 64 V8`。
   - **附带验证（DLL 闭包）**：客户端没有 MSYS2 PATH 也能拿到这个结果 = 依赖闭包打包生效。
2. `db_list_tables {db: B}`
   - **预期**：**空表清单**（`tables: [], count: 0`）——sqlite3/jsonfile 这正是"从空文件开始"的证据；远端库里若已有旧表，先跑 §6 清理再回来。
   - **附带验证（懒创建）**：此刻去看磁盘——`probe.db` 已生成（sqlite 打开即建），`probe.json` **还没有**（jsonfile 首次写入行才物化）。这是报告里的 cwd/懒加载行为。
3. 故意路由错：`db_info {db: nosuch}`
   - **预期**：报错 `-32602`，文案含 `(available: …)` 全部后端清单——"报错即教学"。
4. 问一句"你现在连接的数据库 MCP 服务器有哪些工具、什么时候该用哪个？"——客户端模型能给出正确路由意见 = `instructions` 决策表已注入生效。

> 协议细节（5 个协议版本协商、URI 模板、completion、垃圾输入容错、优雅退出）无法在客户端 UI 里逐个手点，用 §7 的 `run1_protocol.py` 一键覆盖。

---

## 2. 元数据与结构化读（对应 runner2；空表可用性）

在 B 上建一张探测表并灌 3 行。**统一口径**：表 `items`，列 `id / name / price / qty`，行 `(1,alpha,9.5,10) (2,beta,59,2) (3,gamma,12.25,10)`。

1. `db_trans {db: B, sqls:[…建表…]}`
   - sqlite3：`create table items (id text primary key, name text, price real, qty integer)`
   - mysql/postgres：`create table items (id varchar(32) primary key, name varchar(64), price double precision, qty int)`
   - **dm8（重要，F-3）**：必须带 schema 引号、列名也引号小写：`create table "dbtest"."items" ("id" varchar(32) primary key, "name" varchar(64), "price" double precision, "qty" integer)`。裸名 `create table items …` 会"成功"但落在 SYSDBA schema，工具面看不见——这本身就是验证点，想看现象就先试裸名再 `db_list_tables`。
   - **预期**：`status:200`，`Transaction success`。jsonfile 无需此步（表随首行写入自动出现）。
2. `db_describe_table {db: B, table: items}`
   - **catalog 后端（sqlite3/mysql/postgres/dm8，即除 jsonfile 外）**：在建表后、插数前先做一次——**空表也有 4 列完整元数据**（id 标记 `primary_key:true`），catalog 直查不依赖行数。
   - **jsonfile**：表在首行写入前不存在（describe 明确报 `unknown table`）；插完 3 行后再 describe，同样出 4 列（类型为推断值 `string/number`）。
3. 插 3 行：`db_insert`（MRTR 首次确认流程见 §3.0；本节可先逐行批准）
4. 依次做下列读并核对**预期**：

| 操作（db_query / db_count） | 预期结果 |
| --- | --- |
| `id:"2"` | 恰 1 行 beta（jsonfile 弱类型下字符串也能命中数字 id） |
| `price:">,20"` | 恰 1 行 beta（59）。gamma 12.25 未过 20 —— 比较前缀仅顶层列条件生效 |
| `lks:{name:"a"}` | alpha/beta/gamma（子串匹配 a） |
| `ins:{qty:[10,2]}` | 3 行（alpha、gamma qty=10；beta qty=2） |
| `ors:{name:"beta", qty:10}` | 3 行——alpha、gamma 由 `qty=10` 命中，beta 由 `name` 命中（ors = 任意条件命中即入选） |
| `fields:["name"], sort:"price asc"` | alpha → gamma → beta（9.5 → 12.25 → 59） |
| `page:2, size:2, sort:"price asc"` | 第 2 页仅 beta（第 1 页 alpha,gamma），信封带 `pages:2, records:3` |
| `page:5`（不给 size） | 明确报错提醒"page 必须与 size 同给" |
| `qty:10, group:["qty"], sum:"price,tp", count:"*,n"` | `[{qty:10, tp:21.75, n:2}]`；dm8 上 tp/n 也已是数字（归一化） |
| `count:"*"` / `count:["*"]` / `count:{"*":""}` | 全部返回 `[{total:3}]`（**F-4 修复验证**：不再 `near "*": syntax error`） |
| `count:"*,n"`（显式别名） | 仍返回 `[{n:3}]`（显式别名不受 F-4 修复影响） |
| `db_count` + `qty:999`（**放在顶层，不进 where**） | 明确 `unknown argument 'qty'`（**F-1 修复验证**：不再静默全表计数）；改放 `where:{qty:999}` 则 `total:0` |
| `db_count` + `where:{qty:10}` | `total:2` |

5. 后端差异核对（选中谁就核对谁，这些是**正常**差异）：
   - describe 的类型名按方言返回（实测形态）：sqlite3 `TEXT/REAL/INTEGER`；mysql `varchar/double/int`；postgres `character varying/double precision/integer`；dm8 **大写** `VARCHAR/DOUBLE PRECISION/INTEGER`；jsonfile 为推断类型 `string/number`；
   - **dm8 专项（F-11 已修复）**：`count` 匿名 `*` 三形态与 `group+sum+count` 的聚合值在 dm8 上也必须是 JSON **数字**（此前字符串 `"3"` 是 F-4 与 D-12 组合缝隙，已在 dcefe71 之后修复）；
   - jsonfile 弱类型：`id:"2"`（字符串）能命中整型 2；`lks` 是子串匹配、`%` 通配符被剥离。

---

## 3. 写路径与 MRTR 人机确认（对应 runner3；F-10 验证点）

### 3.0 先体验一次完整确认流（默认无 `--yes`，你现在就是这个形态）

1. `db_insert {db: B, table: items, data:{name:"delta"}}`（**不带 id**）
   - **预期**：客户端弹出/转述确认请求——文案为 `Confirm insert on <B>.items (1 row)? Reply with approved:true to proceed (requestState: v1.…)`。**此时数据未写入**。
   - 批准后：返回 `affected:1, id:"<8位hex>"`（zorm 自动生成 8 位 hex id——这也是为什么本手册建表用 text 主键）。
2. `db_insert_batch` 2 行（同 id 覆盖 + 新 id）
   - **预期**：确认文案 `(2 rows)`；批准后 `affected:2`，且**不**返回单条 insertId。
3. `db_update … where:{id:"3"}` → 确认 `(row id=3)`；`db_delete … where:{id:"1"}` → 确认前**有时点数**文案；`db_delete where:{}` → 清全表（有行数预估）。
4. "拒绝"路径：再来一次 insert，在客户端里**拒绝**它。
   - **预期**：返回 `Write cancelled by user.`，行数不变。
5. **F-10 验证**：`db_trans {db:B, sqls:[ 单条结构化 delete ]}`
   - **预期**：确认文案是 `(1 statement)`（单数，带引号小写列时如 `"dbtest"."items"`）；而 2 条时是 `(2 statements)`。

### 3.1 事务原子性

- `db_trans` 提交两条（一条文本 INSERT + 一条结构化 insert）→ count 应 +2；
- 再发一条**必失败**事务（第二条写 `nosuch_table`）→ 报错，count **不变**（回滚）。dm8 上错误文本应**非空**（`Running transaction error: 第1 行附近…`，**F-8 修复验证**，此前是空串）。

### 3.2 对抗路径（照抄请求体，任何工具形态的 MCP 客户端都能做：MCP Inspector 里手工编辑 params）

这一组验证 MRTR 的完整性保障。用 [MCP Inspector]（`npx @modelcontextprotocol/inspector`）或 §7 脚本最方便：

| 手工请求要点 | 预期 |
| --- | --- |
| 拿到一次 staged `requestState` 后，**改一位再重放**（伪造 seal） | 拒绝：`invalid, expired, forged, or was issued for a different tool` |
| 把 insert 的 state 用来确认 update | 同上拒绝（跨工具） |
| 同一个 state 批准执行两次 | 第二次 `no pending … (already executed or expired)`（防重放） |
| `requestState` 放进 `arguments` 内部 | 拒绝并提示"必须放 params 层（arguments 兄弟位）" |
| 丢掉 state 只回 `inputResponses:{confirm:{approved:true}}` | 回落到最新同目标 op，正常执行 |

> 这 5 条在 UI 客户端里不便构造，直接跑 `python .tmp-field/ft3/run3_mrtr.py`（40 断言）即可全覆盖。

---

## 4. 只读 SQL 闸门（对应 runner2 的 db_query_sql 段 + runner9 gate-sweep）

对 B 用 `db_query_sql` 逐条试（**验收口径：拒绝的必须 -32602 且带 READ-ONLY/教学文案；放行的必须真出数据**）：

| 语句 | 预期 |
| --- | --- |
| `select name, price from items where price > ? order by price desc` + `values:[5]` | 正常出参化结果；postgres 上 `?` 会被自动改写为 `$1`（**绑定位序**不变） |
| `-- 注释
select 1 as x /* 尾注释 */` | 放行（语句体仍是单条 SELECT） |
| `pragma user_version`（sqlite3） | 放行，`[{user_version:0}]` |
| `pragma table_info(items)`（sqlite3） | 放行且**行数据完整**（cid/name/type…）（**F-2 修复验证**：此前是 [{}…] 空对象行；jsonfile/mysql/pg/dm8 跳过本行） |
| `update items set qty=0` / `delete from items` / `insert into items …` / `drop table items` / `alter table items …` | 全部拒绝 |
| `select 1; select 2`（多语句） | 拒绝 |
| `with c as (select 1) delete from items`（数据修改型 CTE） | 拒绝（文案点名 `with ... delete`） |
| `explain analyze create table zz as select * from items` | 拒绝（EXPLAIN 只能包 SELECT/WITH/VALUES——它在 pg 上会**真的建表**） |
| `pragma user_version = 42` / `pragma journal_mode(name)` | 拒绝（赋值形态要 `--allow-sql` 的 db_exec_sql） |
| `select * from items; drop table items`（尾随注入） | 拒绝 |

**最终一致性检查**：跑完上表后 `db_count {db:B, table:items}` 行数应与进入本节时相同——所有被拒向量都真的没执行。

---

## 5. 资源、prompt、completion、通知（对应 runner1/runner4）

1. 客户端资源面板读取 `db://guide`（用法手册，>1000 字，含 MRTR 章节）与 `db://schema`（各后端一览）。
2. 读取 `db://schema/<B>/items` → 列出 4 列元数据；读取 `db://schema/nosuchdb` → 教学型报错附 available 清单。
3. **运行期可见性**（必测：报告核心点）：`db_exec_sql`/客户端里建一张**新表** `rtime`，**不重启**直接读 `db://schema/<B>/rtime` → 能读到（URI 模板按读时匹配，不是启动快照）。
4. `db-playbook` prompt 可拉取；completion：输入 `db:` 前缀出现各后端名。
5. 通知（2026 客户端）：订阅后任一写入应触发 `resources/updated`（含 uri）+ `resources/list_changed`。legacy 时代 `resources/subscribe` 是空 stub（成功但无通知）——这是文档化的 mcp-cpp 设计，不是缺陷（F-7）。

---

## 6. HTTP 传输与鉴权（对应 runner5/runner7；可选，stdio 用户可跳过）

给 `<T>` 里同库另起一个 HTTP 实例（stdio 与 HTTP 可共存，文件锁注意别同时写同一 sqlite；建议本节用 jsonfile 独占）：

```powershell
D:\codes\zorm-mcp\bin\zorm_mcp_server.exe --config D:\zorm-test\cfg.json --http 8399 --http-token tk-A --http-token tk-B --http-origin http://good.example
```

逐条验证（curl 或任意 HTTP 客户端，POST `/mcp`，头 `Content-Type: application/json`、`Accept: application/json, text/event-stream`）：

| 请求 | 预期 |
| --- | --- |
| **无** `Authorization` | **401** |
| `Authorization: Bearer WRONG` | **401** |
| `Bearer tk-A` / `Bearer tk-B`（token 可重复注册） | **200**，initialize 结果含 `zorm-mcp` v1.1.1 |
| 加 `Origin: http://evil.example` + 合法 token | **403**（origin 白名单） |
| `Origin: http://good.example` | **200** |
| 发一段非 JSON 文本 | 400 且进程**存活**（再发正常请求成功） |
| 不带会话 `GET /mcp` | **405** |

**HTTP 上的 MRTR 跨主体绑定**：用 token-A 走一次写（得 input_required + state），再用 **token-B** 回放该 state → 拒绝；token-A 本人重放 → 执行成功。

**F-6 回归**：命令行试 `--http bogus` 与 `--sse-keepalive bogus` → 两条都应在启动期 `invalid …` + exit 2（此前 `bogus` 被 atoi 吞成 0 静默继续）。

收尾：`Ctrl+C` 停 HTTP 进程。

---

## 7. CLI / 配置 / 隐藏面抽查（对应 runner4）＋ F-5

1. `--allow-sql` 前提下 spark 一次 `db_exec_sql`：描述文案应说明"返回受影响行数、**不**返回结果行，读数据用 db_query_sql"（**F-5 验证**：读工具列表里 db_exec_sql 的 description）。
2. `drop table` 链路：`db_exec_sql` 建表 → `db_describe_table` 见列 → alter 加列 → describe 多出一列 → drop → 查询报 `no such table`。
3. 未知 flag：`zorm_mcp_server.exe --nope` → stderr 用法文本 + exit 2。
4. 配置模板自污染防线：`--config D:\codes\zorm-mcp\zorm-mcp.json`（模板的 jsonfile connString 指向模板自身）→ 启动即拒绝 + 非零退出，模板文件完好。
5. unicode 抽查：插入 `id:"中文id"`、emoji 值，读回一致。

---

## 8. 一键脚本复跑（自动化对照；人工步骤的完整机器版）

`.tmp-field/ft3/`（gitignored，随本仓库测试现场保留）下每个 runner 与报告章节一一对应，两条命令即可复现"283+45 断言"：

```powershell
cd D:\codes\zorm-mcp\.tmp-field\ft3
python run1_protocol.py  # 协议面   → 预期 35/0
python run2_data.py      # 数据工具 → 预期 97/0
python run3_mrtr.py      # MRTR    → 预期 40/0
python run4_extras.py    # 杂项     → 预期 37/0
python run5_http.py      # HTTP    → 预期 17/0
python run6_remote.py    # 远端矩阵 → 预期 46/0（需 10.0.0.7）
python run7_http_mrtr.py # HTTP-MRTR → 预期 6/0
python run8_fixverify.py # F-1..F-10 修复验证 → 预期 20/0
python run9_gatesweep.py # 门回归   → 预期 25/0
```

每个脚本自带全新空库（跑前自删自建），互不污染；末行打印 `== runnerN: X checks, 0 failed ==` 即通过。

---

## 9. 收尾清理清单

- 客户端/HTTP 进程退出后，删除：`<T>\probe.db`（及 -wal/-shm）、`<T>\probe.json`、`<T>\cfg.json`；
- 远端库清理（在 B 上跑）：`drop table if exists items; drop table if exists rtime;`（dm8 记得加 `"dbtest".` 限定与引号）；
- `claude mcp remove zorm-mcp`（或从客户端配置里删掉条目）；
- 自己实验用 `db_exec_sql`/裸 DDL 建的表若落在远端**登录 schema**（dm8 的 SYSDBA 上大写名），单独 drop。

## 10. 结果判读

- 全部"预期"吻合 → 与 [FIELD-TEST-REPORT-2026-10-23.md](FIELD-TEST-REPORT-2026-10-23.md) §6.1 的回归结论一致：修复后无回归、无新发现。
- 某一步不符 → 记录操作/请求体/报错原文，对照报告 §6 发现清单：若属已记录项按"建议"处理，否则作为新发现补录。
