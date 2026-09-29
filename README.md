# Knowledge

数据库版**知识检索**系统：把代码仓库索引语料（chunk / 调用图 / 数据流）与电子书等文本归一为「条目 + 三元组」，用受控的 **JSE（JSON S-Expression）** 查询语言供 AI Agent 安全访问。

支持两类输入源：**代码仓库**（my_db `code_indexer` 产物）与**电子书**（my_db `import_book` 产物）。

---

## 设计目标

### 一句话

让 AI Agent 能够**安全、准确、受控**地检索和理解大型代码库与文本语料，而不必给它一个数据库连接或让它裸写 SQL。

### 目标一：归一化存储 —— 用两种单元表达一切

信息形态各异（函数、结构体、宏、调用关系、变量数据流、文件归属；书籍、章节、页面……），但存储层只有两种单元：

- **条目（knowledge）**：任意对象 → 一条记录，动态 JSON。
- **三元组（statement）**：任意关系 → 一条边，`subject - predicate - object`。

不为一类信息开一张表，不为一种关系造一套模型。业务差异留在 JSON 内部，新增一类信息 = 按约定写条目，无需 DDL、无需迁移。

### 目标二：受控查询 —— AI 只能表达意图，不能触碰底层

AI **不写 SQL**，只产出受限的 JSE 表达式树；编译器负责翻译为参数化 SQL，并在编译期统一注入活跃口径（`AND is_active`）与安全校验。

- **有限**：摸不到任意 SQL，做不了 `DROP` / 任意 JOIN / 绕过行级口径；每次查询是可序列化、可记录、可审查的表达式树。
- **灵活**：条件组合、JSONB 深层过滤、全文、向量、聚合、多跳图遍历都在同一表达式体系内完成。

### 目标三：面向 AI Agent —— 工具化、宿主中立

JSE 被设计为 Agent 的工具接口：同一套知识库可通过插件接入任意 Agent 宿主（DSH / Claude Code / opencode……），而不是锁定在单一平台内。

- 查询有确定语义、口径有默认保障；
- 动词抽象为业务算子（`$meta` / `$fti` / `$triple` / `$k-hop`），Agent 说"要什么"，算子决定"怎么算"；
- 不实现循环、分支、定义的"可控不完备"，使其始终可静态分析。

### 目标四：可编译、可部署、隐私优先

核心引擎用 **C++17 + libpq + jansson** 实现，`make` 产出单文件二进制（~150KB），无运行时依赖：

- 与 `my_db` 工具链一致，沿用「本地编译 → 二进制分发 → 直接运行」的部署方式；
- 代码与查询不出机器，适合企业内网与隐私敏感场景。

### 目标五：本体可演化 —— 规范也是数据

约束不靠外置 schema 文件，而是允许把**信息规范本身**写成条目（schema-as-data），与事实共享同一套存储、检索、权限与历史机制。规范的演进就是条目的演进，天然带有版本与归档。

### 目标六：语义与方言分离 —— 资产长期沉淀

JSE 算子是 AI 与存储层之间的**契约**：存储引擎（PostgreSQL / 未来的检索或图引擎）只是它当前的编译目标。JSE 指令集稳定后，后端迁移不使历史查询资产贬值。

### 非目标

- 不是通用图数据库，也不引入专用图引擎（图遍历用 PostgreSQL 递归 CTE）。
- 不是静态分析器/解析器——代码解析、调用图、数据流由 `my_db` 索引管线产出，电子书由 `import_book` 产出，本项目负责归一与查询。
- 首批不做写侧 Action（声明式受控写）+ 规范强制校验 + 迁移对账，属后续演进。

---

## 与原生 code search（my_db）的区别与优势

**关系**：不是替代，而是叠加。原生 `my_db` 负责**抽取与检索**（ctags/AST 解析、GPU 向量、HNSW、调用图、数据流）；
本项目复用它的全部产物，在其上加一层**关系化、可组合、可共享、Agent 友好的知识/查询层**。

| 维度 | 原生 my_db | 本项目 |
|---|---|---|
| 语义搜索 | 向量（+TF-IDF boost） | 向量 + 全文 **RRF 融合**，召回更稳 |
| 关系模型 | `call_graph.json`/`dataflow.json` 静态文件，按名字查一层 | 边入库有索引，**任意深度/方向/谓词多跳**，可返回路径序列 |
| 查询组合 | 多个子命令，各自独立 | 一条 JSE 内组合：全文 + 向量 + 元数据 + 图约束 + `AND/OR/NOT` + 投影/分页 |
| 跨项目/跨源 | `cross-search` 脚本聚合独立搜索 | 代码 + 电子书 + 多项目**同一模型**，可 join |
| 输出 | CLI 文本，需再解析 | 结构化 JSON，**token 可控**（`--no-content`/字段裁剪/limit） |
| 安全/受控 | 开放命令 | JSE + 全参数化 + 路径白名单 + 口径统一注入，**可审计** |
| 多用户 | 每人一份本地缓存 | 无状态查询层 + PG，**共享知识库**，可起只读 HTTP API |
| 持久/并发 | 单进程 mmap 文件 | PG ACID、并发读、事务、标准备份 |
| 数据演进 | 文件格式固定 | 条目动态 JSON，新信息类型免迁移；谓词/规范本身是数据 |

**具体优势**

1. **多跳与图查询**：原生只能"某函数的直接调用者"；本项目能问"A 经 2~3 跳能到谁""双向调用链""跨项目路径"——因为边是带索引的行，而非 JSON 里的数组。
2. **一次查询跨信号组合**：例如"`lang=c` 且全文含 `page_alloc` 且从 X 起 2 跳可达"，原生需多次调用再自行求交。
3. **跨源连接**：代码与电子书同一张图，可做"这本书的主题相关代码"这类跨源查询。
4. **Agent 友好**：单一受控 DSL，不必让模型学多个 shell 命令、解析文本；token 开销可量化裁剪；`context` 一次给全（定义+调用关系+路径+语义近邻）。
5. **可共享、可分析、可持久化**：PG 让知识库成为可 SQL 分析、可事务更新、可备份、可对外只读服务的数据资产。

**原生仍更好的地方（诚实说）**

- **纯单项目语义搜索更快**：无导入、无 DB 往返。
- **抽取质量由原生决定**：本项目不提升代码解析；向量引擎也是原生的。
- **零运维**：原生是一个二进制；本项目需要 PostgreSQL + 导入 + key 对齐（个人用已优化：Linux 全量 ~200s / 271MB）。

**结论**：只要"用自然语言找到一个函数"，原生够用且更快；本项目值钱之处在于**把检索结果变成可关系化查询、可组合、可跨源、可安全共享给 Agent 的知识库**。

---

## 项目状态

| 模块 | 状态 |
|---|---|
| PostgreSQL DDL（双表 + 生成列 + 索引） | ✅ 已落地 |
| JSE 编译器（全部算子，参数化 + 白名单） | ✅ 完成 |
| 应用层分词 + 全文检索 | ✅ 完成 |
| 外挂向量引擎接入（provider 契约 + 适配器） | ✅ 完成 |
| `search` / `context` 上下文供给 | ✅ 完成 |
| 代码导入器（chunks / call_graph / dataflow → 条目+三元组） | ✅ 完成（redis 全量 8434 条目 + 3.6 万三元组，2.3s） |
| 电子书导入器（`/opt/books` Markdown → 条目+三元组） | ✅ 完成（7 本 / 2445 页 / 3382 边，2.5s） |
| 一键流水线 `tools/ingest.sh`（分析 + 入库） | ✅ 完成 |
| token 友好输出（嵌套 meta/content + 字段裁剪 + `--no-content`） | ✅ 完成 |
| 混合检索（向量 + 全文 RRF 融合） | ✅ 完成 |
| 编译期 key→id 解析、`$k-hop` 结果上限、回归测试 | ✅ 完成 |
| 导入优化（流式 + 内存受限 + 大项目批量重建索引） | ✅ 完成 |
| HTTP API（只读，连接池，共享知识库） | ✅ 完成 |
| 多项目批量导入（16 仓库 / ~250 万 chunk） | 🚧 进行中 |
| smoke test / 迁移对账 / 规范校验 / Action 层 | ⏳ 待办 |

> 关键路径已打通：`cache_query` 语义搜索 → provider → key 归一 → PG join → `context`（代码与电子书两路均已端到端验证）。详见 `design.md` §8。

---

## 架构决策（已锁定）

| 维度 | 决策 |
|---|---|
| 存储 | PostgreSQL 16，`knowledge`（节点）+ `statement`（边）双表 |
| 主键 | 条目 `BIGSERIAL id`；三元组端点为 `BIGINT` 代理键（`subject_id/predicate_id/object_id`） |
| 状态 | 单轨 `is_active` STORED 生成列（`NOT is_archived AND end_time IS NULL`），与 partial index 谓词逐字一致 |
| 索引 | 正向/反向遍历 partial index、热字段表达式索引、`search_tsv` GIN 全文索引 |
| 向量 | **外挂引擎**（沿用 `.hnsw` 文件），向量不入 PG；`$search` 编译为候选 key 的 `VALUES` CTE |
| 全文 | **应用层预分词**（snake_case / camelCase 拆分）写入 `search_tsv`，查询用 `to_tsquery('simple', ...)` |
| 编译器 | C++17 + libpq + jansson，`make` 出单文件二进制 `knowledge` |
| 安全 | 路径正则白名单、标识符不可参数化部分强校验、值全部 `$n` 绑定 |

---

## 目录

```
schema.sql            # DDL（PG16）
Makefile              # 编译单一二进制
src/tokenize.hpp/.cpp # 代码标识符分词器
src/compile.hpp/.cpp  # JSE 校验 + 编译器
src/vector.hpp/.cpp   # 外挂向量引擎 provider 调用
src/importer.hpp/.cpp # 代码知识导入器（chunks/call_graph/dataflow）
src/books.hpp/.cpp    # 电子书导入器（/opt/books 的 Markdown）
src/db.hpp/.cpp       # libpq 访问
src/api.hpp/.cpp      # search / context 可复用实现（CLI 与 HTTP 共用）
src/server.hpp/.cpp   # HTTP API 服务（只读，连接池）
src/main.cpp          # CLI（tokenize/compile/query/search/context/import/import-books/serve/seed）
third_party/httplib.h # vendored header-only HTTP 库（MIT）
tools/ingest.sh               # 一键流水线：代码分析 + 入库
tools/vector_provider.sh      # 代码向量引擎适配器（my_db cache_query）
tools/vector_provider_books.sh# 电子书向量引擎适配器（my_db book cache）
tools/vector_provider_stub.sh # 测试用 stub
prompts/              # 向 Google AI 提问的三弹材料 + 统一 brief
design.md             # 技术设计文档 + 任务进度
```

---

## 运行

```bash
# 1. 依赖（Ubuntu）
sudo apt-get install -y libpq-dev libjansson-dev

# 2. PG 初始化（首次）
psql -h 127.0.0.1 -U knowledge -d knowledge -f schema.sql

# 3. 编译
make

# 4. 灌入演示数据
./knowledge seed

# 5. 查询（JSE 从 stdin）
echo '{"$where":{"$and":[{"$meta":{"path":"kind","$eq":"function"}},{"$fti":"page_alloc"}]},"$project":["key","meta.symbol"],"$limit":20}' | ./knowledge query

# 6. 只编译看 SQL（不连库）
echo '{"$where":{"$fti":"page_alloc"}}' | ./knowledge compile

# 7. 分词
./knowledge tokenize "__alloc_pages_slowpath AllocPageSlow"

# 8. 语义搜索（外挂向量引擎 + PG 标量过滤）
export KNOWLEDGE_VECTOR_CMD="./tools/vector_provider.sh"
export ANALYSIS_DIR=/opt/code_caches/redis_cache PROJECT=redis ROOT=/opt/redis
./knowledge search "memory pool allocation" --k 5

# 9. 一次性取完整上下文包（定义 + 调用关系 + 调用路径 + 语义近邻）
./knowledge context /code/local/redis/src/module.c/RM_PoolAlloc --depth 2

# 10. 回归测试
make test
```

连接串通过 `DATABASE_URL` 覆盖，默认 `postgres://knowledge:knowledge@127.0.0.1:5432/knowledge`。

---

## 代码知识库使用（完整流程）

```
① 分析（my_db：ctags/向量/调用图/数据流）  →  ② 入库（importer，零重算）  →  ③ 语义化查询
```

### ① 一键分析 + 入库

```bash
# 自动 index + vector + hnsw + import
./tools/ingest.sh /opt/sqlite

# 已有分析结果，只入库
./tools/ingest.sh /opt/sqlite --project sqlite --skip-analyze
```

`ingest.sh <repo>` 默认：项目名取目录名，缓存目录 `/opt/code_caches/<project>_cache`，
完成后自动给出后续查询要用的环境变量。

### ② 语义搜索

```bash
export KNOWLEDGE_VECTOR_CMD="./tools/vector_provider.sh"
export ANALYSIS_DIR=/opt/code_caches/redis_cache PROJECT=redis ROOT=/opt/redis

./knowledge search "memory pool allocation" --k 5
# -> [{ key, meta.symbol, .file, .line, .signature,
#       rrf, vector_rank, vector_score, lexical_rank }]
```

`search` 做**混合检索**：向量候选（外挂引擎）+ 全文候选（`$fti`），用 RRF 融合排序。

典型过程：**先用一段自然语言描述功能**，拿到候选 `key`，再对目标 key 取上下文。

### ③ 取上下文（一次拿全）

```bash
./knowledge context /code/local/redis/src/module.c/RM_PoolAlloc --depth 2

# 省 token：不要正文 / 限制正文字节数
./knowledge context <key> --no-content
./knowledge context <key> --max-code-bytes 800
```

`context` 返回：定义（完整源码/正文）+ callers + callees + 调用路径（节点序列）+ 语义近邻，
用 `--depth` / `--k` / `--no-content` / `--max-code-bytes` 控制 token 开销。

### ④ 结构化查询（JSE）

```bash
echo '{"$where":{"$and":[{"$meta":{"path":"kind","$eq":"function"}},
       {"$fti":"page_alloc"},
       {"$k-hop":{"from":"<key>","predicates":["/pred/calls"],"depth":2}}]},
       "$project":["key","meta.symbol"],"$limit":20}' | ./knowledge query
```

---

## 电子书知识库（第二类输入源）

`my_db` 的电子书系统（`import_book` → `/opt/books/{book}/chapters/**/page_*.md` + `books_{name}.jina.bin`）
也可作为知识库输入源，**key 天然对齐**（`cache_query` 返回的 `name` 即 page key）。

```bash
# 入库（全部书，或指定一本）
./knowledge import-books
./knowledge import-books --books-dir /opt/books --book ddia

# 语义搜索书籍内容
export KNOWLEDGE_VECTOR_CMD="./tools/vector_provider_books.sh"
./knowledge search "重建圣殿" --k 3

# 取上下文（book -> chapter -> page 的 contains 关系）
./knowledge context "/books/耶路撒冷三千年/chapters/94-瓦利德：天启与奢侈/page_0000" \
  --depth 1 --predicate /pred/contains --no-content
```

入库结构：

```
/books/{book}                                  kind=book      {title, author}
/books/{book}/chapters/{chapter}               kind=book_chapter  {order}
/books/{book}/chapters/{chapter}/page_NNNN     kind=book_page     {book,chapter,page,file}
关系：book -contains-> chapter -contains-> page
```

> 书籍内容以中文为主，语义搜索走向量（`$search`）；全文检索 `$fti` 对中文能力有限。

---

## 上下文供给（对外契约）

本项目是**上下文/证据供给层**（代码 + 电子书）：语料生成、微调等由外部系统负责，本项目只提供结构化上下文。

### `knowledge context <key> [--depth N] [--k N] [--predicate <key>]`

一次返回一个节点（代码符号 / 书籍页）的完整上下文包：

```json
{
  "key": "...",
  "definition": [ { "key", "meta", "content", "version", "updated_at" } ],
  "callers":  [ { "key", "meta" } ],
  "callees":  [ { "key", "meta" } ],
  "paths":    [ { "depth": 2, "keys": "{A,B,C}" } ],
  "related":  [ { "key", "meta", "score" } ]
}
```

- `definition`：完整源码 / 正文（不截断）+ 元数据 + 版本
- `callers` / `callees`：直接关系（代码为 `calls`，书籍为 `contains`）
- `paths`：关系路径的**节点序列**（从起点到终点），带深度
- `related`：**语义搜索近邻**（来自外挂向量引擎，按 score 降序）

### `knowledge search <query> [--k N] [--where <jse>]`

混合检索：向量候选（外挂引擎）+ 全文候选（`$fti`）→ **RRF 融合** → PG 标量过滤。
结果带 `rrf` / `vector_rank` / `lexical_rank` / `vector_score`，便于判断命中来源。

### 向量引擎接入（provider 契约）

向量搜索走外挂引擎。provider 是一个命令，接口为：

```
<cmd> '<query>' <k>   ->   {"results":[{"key":"...","score":0.91}, ...]}
```

通过 `--vector-cmd` 或环境变量 `KNOWLEDGE_VECTOR_CMD` 指定。已提供真实适配器 `tools/vector_provider.sh`，包装 `my_db` 的 `cache_query --analysis-dir` 并映射为 Knowledge key：

```
key = /code/local/{project}/{file-relative-to-root}/{symbol}
```

例：`ANALYSIS_DIR=/opt/code_caches/redis_cache PROJECT=redis ROOT=/opt/redis` 时，
`/opt/redis/src/module.c` 的 `RM_PoolAlloc` → `/code/local/redis/src/module.c/RM_PoolAlloc`。

电子书用 `tools/vector_provider_books.sh`（`BOOK_CACHE=/book/cache`），`cache_query` 返回的 `name` 直接就是 page key，无需映射。

> 说明：`related` / `search` 要命中，PG 中必须有同 key 的条目（即先跑导入器）。当前 `seed` 只灌了少量演示数据。

---

## HTTP API（只读）

查询层无状态、状态在 PG，因此天然支持多调用方共享一个知识库。服务只读：写/改/删只在本地导入。

```bash
# 准备只读角色（首次，本地执行）
sudo -u postgres psql -c "CREATE ROLE knowledge_ro LOGIN PASSWORD 'knowledge_ro';"
sudo -u postgres psql -d knowledge -c \
  "GRANT CONNECT ON DATABASE knowledge TO knowledge_ro;
   GRANT USAGE ON SCHEMA public TO knowledge_ro;
   GRANT SELECT ON ALL TABLES IN SCHEMA public TO knowledge_ro;
   ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT ON TABLES TO knowledge_ro;"

# 启动（默认 127.0.0.1:8931，连接池 4）
./knowledge serve --port 8931 --ro
export KNOWLEDGE_VECTOR_CMD=./tools/vector_provider.sh
```

| 端点 | 说明 |
|---|---|
| `GET /health` | `{"status":"ok"}` |
| `GET /stats` | knowledge / statement / active 计数 |
| `POST /jse` | body 为 JSE 查询（可带 `"$vectors":[...]` 供 `$search`） |
| `GET /search?q=&k=&where=` | 混合检索 |
| `GET /context?key=&depth=&k=&predicate=&no_content=&max_code_bytes=` | 上下文包 |

```bash
curl -s -X POST localhost:8931/jse \
  -d '{"$where":{"$and":[{"$meta":{"path":"kind","$eq":"function"}},{"$fti":"zmalloc"}]},"$project":["key","meta.symbol"],"$limit":5}'
curl -s 'localhost:8931/search?q=memory%20pool&k=5'
curl -s -G 'localhost:8931/context' --data-urlencode 'key=/code/local/redis/src/module.c/RM_PoolAlloc' --data 'depth=2'
```

限制：请求体 ≤ 1MB、每请求 `statement_timeout=30s`、`$limit` ≤ 1000、`$k-hop` depth ≤ 5；
只读角色在数据库层禁止任何写操作。

---

## JSE 算子（当前实现）

- `$and` / `$or` / `$not`
- `$meta`：`path` + `$eq/$ne/$gt/$gte/$lt/$lte/$in/$nin/$exists`
- `$fti`：全文（应用层分词，OR 语义）
- `$search`：向量（外挂引擎提供 `vectors` 候选，按 `$search_score` 排序）
- `$triple`：`subject` / `predicate` / `object` 边匹配；`direction`（`out` 沿指定 subject 的出边 / `in` 入边 / `both` 默认两端）
- `$k-hop`：`from` / `predicates` / `depth` / `direction` / `where`，ID 拓扑遍历 + path 防环
- 排序键：`$search_score`（需 `$search`）、`$fti_rank`（需 `$fti`，用 `ts_rank`）
- 修饰符：`$project` / `$order` / `$limit` / `$offset`

### 路径约定

- `$meta.path` 相对 `meta` 根节点（`lang` → `meta->>'lang'`）
- `$project` / `$order` 路径从行根起算（`key`、`meta.symbol`、`content.code`）

---

## 后续演进

- **规范条目校验**：`meta.category=spec` 定义字段约束，写入校验与编译器共享路径白名单
- **LLM 报错闭环**：语法 / 安全 / 规范三层结构化 Error Payload + System Prompt
- **Action 层（写侧）**：声明式动作 + 前置校验 + 归档/snapshot + 乐观锁
- **迁移对账**：旧 mmap 系统与新 PG 系统的双跑 diff 与灰度切流
