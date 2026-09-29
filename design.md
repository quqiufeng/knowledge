# Knowledge — 技术设计文档（design.md）

> **可查询的知识底座**：把代码、电子书等异构语料归一为「条目 + 三元组」，用受控的 JSE 查询语言提供 语义+全文+图+聚合 的一体化查询。**code search 是它的第一个应用，不是全部。**
> 本文记录技术架构、关键决策与任务进度。更新于 2026-09-29。

---

## 1. 项目定位

现有 `my_db`（/opt/my_db）是一套 C 引擎 + mmap KV + 外挂 HNSW 的本地语义搜索系统，提供两套产物：

- **代码**：`chunks_meta.jsonl`、`call_graph.json`、`dataflow.json`、`*.jina.bin`
- **电子书**：`/opt/books/{book}/chapters/**/page_*.md`、`books_{name}.jina.bin`

本项目将其**知识层**迁移并升级为数据库化的 Ontology 系统：

- **归一存储**：任意业务对象 → 条目（动态 JSON）；任意关系 → 三元组。仅两张表。
- **受控查询**：AI 不写 SQL，只产出 JSE（JSON S-Expression），由编译器翻译为参数化 SQL 并注入口径/权限。
- **可演化本体**：规范、谓词定义、Action schema 本身就存为条目（schema-as-data）。
- **持久与可审计**：历史（归档 + snapshot 三元组）是一等数据。

对照对象：Palantir Ontology（schema-first）。本项目取 data-first 路线——数据先行，本体作为数据的一部分生长。

### 与原生 code search（my_db）的关系

不是替代，而是叠加。my_db 负责**抽取与检索**（解析、GPU 向量、HNSW、调用图、数据流）；本项目复用其全部产物，在上面加一层**关系化、可组合、可共享、Agent 友好的查询/知识层**。差异要点：

- **关系**：原生只有静态 `call_graph.json`/`dataflow.json` 的一层查表；本项目把边入库索引，支持任意深度/方向/谓词的多跳与路径序列。
- **查询**：原生是多个独立子命令；本项目是一条 JSE 内组合全文+向量+元数据+图约束+布尔逻辑。
- **范围**：原生按项目/命名空间隔离；本项目把代码、电子书、多项目放进同一模型，可 join。
- **形态**：原生每人一份本地缓存文件；本项目是共享 PG + 可选只读 HTTP API。
- **原生的优势仍在**：纯单项目语义搜索更快、零运维；本项目不提升代码解析，向量引擎也是原生的。

详细对照见 `README.md` 的「与原生 code search（my_db）的区别与优势」。

---

## 2. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│ 消费面                                                       │
│   AI Agent 插件（宿主中立）  · Web / CLI                     │
└───────────────┬─────────────────────────────────────────────┘
                │ JSE 表达式（JSON AST）
┌───────────────▼─────────────────────────────────────────────┐
│ 查询层（C++）                                                 │
│   JSE 校验 → AST → 编译器 → 参数化 SQL                        │
│   路径白名单 · 值参数绑定 · 口径注入（AND is_active）          │
└───────┬───────────────────────────────────┬─────────────────┘
        │                                   │
┌───────▼───────────┐             ┌─────────▼─────────────────┐
│ 外挂向量引擎       │             │ PostgreSQL 16             │
│  .hnsw / USearch  │             │  knowledge（节点）         │
│  $search 候选 key  │             │  statement（边）           │
└───────────────────┘             │  search_tsv（应用层分词的   │
                                  │   tsvector + GIN）          │
                                  └─────────▲─────────────────┘
                                            │ 受控写入管线
┌───────────────────────────────────────────┴─────────────────┐
│ 输入源（my_db 产物，零重算）                                  │
│  代码：chunks_meta.jsonl · call_graph.json · dataflow.json    │
│  电子书：/opt/books/**/page_*.md（book/chapter/page + contains）│
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 数据模型

### 3.1 两张表

| 表 | 角色 | 关键列 |
|---|---|---|
| `knowledge` | 节点 / 条目 | `id`、`key`（全表唯一）、`meta` JSONB、`content` JSONB、`search_tsv`、`version`、`start_time/end_time`、`is_archived`、`is_active` |
| `statement` | 边 / 三元组 | `id`、`subject_id/predicate_id/object_id`（BIGINT 代理键）、`meta`、`start_time/end_time`、`is_archived`、`is_active` |

### 3.2 代码域映射

| my_db 数据 | Knowledge 形态 |
|---|---|
| chunk（函数/结构体/宏） | 条目 `key=/code/{proj}/chunks/{file}/{symbol}`，`meta={kind,lang,file,line,symbol}`，`content={code}` |
| 符号索引 | 条目（由 chunk 条目承担） |
| `call_graph.json` | 三元组 `A -calls→ B` |
| `dataflow.json`（DEF/SET/USE/字段） | 三元组 `defines/assigns/uses/has_field` |
| import/export 边 | 三元组 `imports/exports` |
| 谓词定义 | 条目 `meta.category=predicate`，key 形如 `/pred/calls` |
| 规范定义 | 条目 `meta.category=spec`（规划中） |

### 3.3 电子书域映射

`knowledge import-books` 读取 my_db 电子书系统产出的 Markdown（`/opt/books/{book}/chapters/**/page_*.md`）：

| 电子书产物 | Knowledge 形态 |
|---|---|
| `_meta.json` | 条目 `/books/{book}`，`meta={kind:book, title, author}` |
| 章节目录 | 条目 `/books/{book}/chapters/{chapter}`，`meta={kind:book_chapter, order}` |
| `page_NNNN.md` | 条目 `/books/{book}/chapters/{chapter}/page_NNNN`，`meta={kind:book_page, book, chapter, page, file}`，`content={text}` |
| 归属关系 | 三元组 `book -contains→ chapter -contains→ page`（谓词 `/pred/contains`） |

**key 天然对齐**：my_db 的 `cache_query` 对书籍返回的 `results[].name` 正是上述 page key，
因此 `tools/vector_provider_books.sh` 只需透传 `name`→`key`，无需路径映射。

> 全文：书籍正文以中文为主，`search_tsv` 直接由原文经 `to_tsvector('simple', ...)` 生成（不经代码分词器）；
> 中文检索主要依赖语义搜索 `$search`。

### 3.4 状态单轨与有效期

```sql
is_active BOOLEAN NOT NULL
  GENERATED ALWAYS AS (NOT is_archived AND end_time IS NULL) STORED
```

- 选择 `STORED` 生成列而非时间函数：`now()` 是 STABLE，**不能**用于生成列（PG 要求 IMMUTABLE），因此定义收窄为「未归档且未设终止时间」。
- 若将来需要「预设未来失效」语义，改由受控写入管线维护普通列或触发器。
- 编译器默认注入 `AND is_active`，与 partial index 谓词**逐字一致**，保证索引命中。

### 3.5 索引

```sql
-- 图遍历
CREATE INDEX idx_stmt_fwd ON statement (subject_id, predicate_id, object_id) WHERE is_active;
CREATE INDEX idx_stmt_rev ON statement (object_id, predicate_id, subject_id) WHERE is_active;
CREATE INDEX idx_stmt_pred ON statement (predicate_id) WHERE is_active;
-- 条目热字段
CREATE INDEX idx_knowledge_kind   ON knowledge ((meta->>'kind'))   WHERE is_active;
CREATE INDEX idx_knowledge_lang   ON knowledge ((meta->>'lang'))   WHERE is_active;
CREATE INDEX idx_knowledge_symbol ON knowledge ((meta->>'symbol')) WHERE is_active;
CREATE INDEX idx_knowledge_file   ON knowledge ((meta->>'file'))   WHERE is_active;
-- 全文 / JSONB
CREATE INDEX idx_knowledge_fti  ON knowledge USING GIN (search_tsv) WHERE is_active;
CREATE INDEX idx_knowledge_meta ON knowledge USING GIN (meta jsonb_path_ops) WHERE is_active;
```

---

## 4. JSE 查询语言

### 4.1 形态

AI 只产出 JSON AST，编译器负责翻译与安全。示例：

```json
{
  "$and": [
    { "$meta": { "path": "lang", "$eq": "c" } },
    { "$fti": "page_alloc" },
    { "$k-hop": { "depth": 2, "predicates": ["/pred/calls"],
                  "where": { "$meta": { "path": "kind", "$eq": "function" } } } }
  ],
  "$project": ["key", "meta.symbol", "meta.file"],
  "$order": { "$search_score": "desc" },
  "$limit": 20
}
```

### 4.2 算子清单

| 类别 | 算子 |
|---|---|
| 逻辑 | `$and` `$or` `$not` |
| 属性 | `$meta`（`path` + `$eq/$ne/$gt/$gte/$lt/$lte/$in/$nin/$exists`） |
| 全文 | `$fti`（应用层分词，OR 语义） |
| 向量 | `$search`（外挂引擎候选 key，配合 `$search_score` 排序） |
| 关系 | `$triple`（`subject`/`predicate`/`object`）、`$k-hop`（`from`/`predicates`/`depth`/`direction`/`where`） |
| 修饰 | `$project` `$order` `$limit` `$offset` |

### 4.3 路径约定

- `$meta.path` 相对 `meta` 根（`lang` → `meta->>'lang'`）
- `$project` / `$order` 从行根起算（`key`、`meta.symbol`、`content.code`）

### 4.4 图遍历编译（核心）

`$k-hop` 编译为递归 CTE，遵循「仅传整型 ID 拓扑 + path 防环 + 深度硬限 + Late Binding」：

```sql
knowledge.id IN (
  WITH RECURSIVE chain AS (
    SELECT k.id AS current_id, 0 AS depth, ARRAY[k.id] AS path
    FROM knowledge k WHERE k.key = $1 AND k.is_active
    UNION ALL
    SELECT st.object_id, c.depth + 1, c.path || st.object_id
    FROM statement st JOIN chain c ON st.subject_id = c.current_id
    WHERE c.depth < $2
      AND st.predicate_id IN (SELECT id FROM knowledge WHERE key = ANY($3::text[]))
      AND st.is_active
      AND NOT (st.object_id = ANY(c.path))
  )
  SELECT c.current_id FROM chain c JOIN knowledge k ON k.id = c.current_id
  WHERE c.depth > 0 AND k.meta->>'kind' = $4
)
```

- `depth` = 边数（anchor 从 0 计，结果排除起点）
- `direction: out` 沿 subject→object（callees），`in` 反向（callers）
- 环路检测用 `path` 数组，防函数互递归死循环

---

## 5. 检索层

### 5.1 全文（应用层预分词）

代码标识符（`__alloc_pages_slowpath`、`AllocPageSlow`）无法被默认分词器友好切分，故在**应用层**分词后写入 `search_tsv`：

1. 按非字母数字切词，保留整个标识符
2. 拆分 snake_case（`_`）与 camelCase
3. 去停用词、小写、去重
4. 写入 `to_tsvector('simple', tokens.join(' '))`

查询侧对 `$fti` 文本做同样分词，构造 `token1 | token2 | ...` 的 OR 查询：
`search_tsv @@ to_tsquery('simple', $n)`。

效果：搜 `page_alloc` 可命中 `__alloc_pages_slowpath`（`page` / `alloc` 子词）。

### 5.2 向量（外挂引擎）

- 向量不入 PG，沿用 my_db 现有 `.jina.bin` + `.hnsw`（768 维）。
- JSE `$search` 执行流程：应用层调用外挂引擎得到候选 `[{key, score}]` → 作为 `VALUES` CTE 传入编译器 → SQL 侧与标量过滤求交、按 `$search_score` 排序。
- 优点：向量更新/重建语义沿用现有工具；PG 只存标量，规模可控。

**Provider 契约**：外挂引擎以命令形式接入，接口固定为

```
<cmd> '<query>' <k>   ->   {"results":[{"key":"...","score":0.91}, ...]}
```

由 `--vector-cmd` / `KNOWLEDGE_VECTOR_CMD` 指定。真实适配器 `tools/vector_provider.sh` 包装 my_db 的 `cache_query --analysis-dir`，并做 key 归一：

```
key = /code/local/{project}/{file-relative-to-root}/{symbol}
```

C++ 侧 `src/vector.cpp` 用 `popen` 调用并解析，结果喂给 `compile_query` 的 `vectors`。

### 5.3 上下文供给（对外契约）

`knowledge context <key> --depth N` 一次返回符号的完整上下文包，供外部语料/微调系统消费：

| 字段 | 内容 | 来源 |
|---|---|---|
| `definition` | 完整源码 + meta + version | `knowledge` 表 |
| `callers` / `callees` | 直接调用关系 | `statement` 表 |
| `paths` | 调用路径节点序列（起点→终点，带 depth） | 递归 CTE（text[] 累积 key） |
| `related` | 语义近邻 + score | 外挂向量引擎 + PG 回填 |

`knowledge search <query>` 则是检索出口。二者构成"本项目只供上下文、不产语料"的边界。

### 5.4 混合检索（RRF）

`search` 同时取两路候选并融合：

| 路 | 来源 | 排序依据 |
|---|---|---|
| 向量 | 外挂引擎（provider） | 余弦相似度 |
| 全文 | PG `search_tsv` + `to_tsquery` | `$fti_rank`（`ts_rank`） |

融合用 **RRF（Reciprocal Rank Fusion）**：`score(key) = Σ 1/(60 + rank_i)`，取 Top-K 后过 PG 标量过滤。
结果附带 `rrf` / `vector_rank` / `lexical_rank` / `vector_score`，便于判断命中来源与调试。
RRF 让"向量召回但全文未命中"与"全文精确但向量偏离"两类结果互补，优于单一排序。

### 5.4.1 查询算子扩充

- `$meta` 增加 `$like` / `$ilike` / `$prefix`：路径/符号的前缀与模糊匹配（值仍走参数绑定）。
- `$key`：对条目 `key` 施加同一组运算符（按项目/路径前缀过滤）。
- `$count: true`：返回匹配总数（聚合）。
- `$group_by: "<path>"`：分组计数，按 count 降序（如 `meta.file` → 文件函数数）。
- 排序键 `$in_degree` / `$out_degree`：按边度数排序（"被调用最多的函数"）。
- 已知 key 在编译期解析为整数 id（`resolve_key`），省去每个条件的子查询。

### 5.5 HTTP API（只读）

查询层无状态（`compile_query` 是纯函数），状态全在 PG，因此天然支持多调用方共享同一知识库。
`knowledge serve` 用 vendored 的 `cpp-httplib`（header-only）暴露只读接口：

| 端点 | 实现 |
|---|---|
| `GET /health` | 存活 |
| `GET /stats` | 计数 |
| `POST /jse` | JSE 编译 + 执行（body 可带 `$vectors` 供 `$search`） |
| `GET /search` | `api_search`（RRF） |
| `GET /context` | `api_context` |

- **连接池**：libpq 连接非线程安全 → `Pool` 维护 N 个连接，租借/归还；PG 侧并发。
- **只读**：服务以 `knowledge_ro`（仅 SELECT）连接；写/改/删只在本地导入。API 层不含写接口。
- **限额**：请求体 ≤ 1MB、`statement_timeout=30s`、`$limit ≤ 1000`、`$k-hop depth ≤ 5`。
- `search` / `context` 逻辑抽到 `src/api.cpp`，CLI 与 HTTP 共用，避免两套实现漂移。

---

### 5.6 Agent 记忆（受控写 Action，本地）

Agent 把本库当长期记忆。写入是**受控 Action**（本地 CLI），非裸写，全部 `src/memory.cpp`：

| Action | 语义 |
|---|---|
| `remember` | 追加情景事件 `/mem/{agent}/events/{ts}-{n}`，`--about` 建 `/pred/about` 边 |
| `fact` | 键值事实 `/mem/{agent}/facts/{topic}`；覆盖前把旧值写 `/@archive/{version}` 并建 `/pred/supersedes` |
| `link` | 在两个已存在条目间建关系 |
| `forget` | 归档（`is_archived=true, end_time=now()`），不物理删除；默认限 `/mem/` |

- **命名空间**：`/mem/{agent}/...` 与 `/code/...` 隔离。
- **Provenance**：自动注入 `meta.agent/session/ts`。
- **历史是一等数据**：覆盖即归档，可回溯。
- **口径注入**：查询默认 `AND is_active`（`$include_archived:true` 关闭）。

### 5.7 通用实体导入

`knowledge import-records <file> [--format jsonl|csv] [--key-field key] [--kind K] [--text-field F] [--prefix P]`：
把任意 JSONL/CSV 记录转为条目。顶层标量字段进 `meta`（可过滤），整条进 `content.value`
（或 `--text-field` 时该字段作 `content.text` 并走全文）。使底座不再只限于 my_db 产物。

### 5.8 向量进 PG（pgvector）

两种语义检索并存：

- 外挂 provider：沿用 my_db `.hnsw`（零导入）。
- **pgvector**：`embedding VECTOR(768)` 列 + HNSW（`vector_cosine_ops`）。`import-vectors` 按
  `chunks_meta.jsonl` 与 `.bin` 顺序对齐灌入；`$knn` 算子编译为 `embedding <=> $vec::vector`，
  与标量/图过滤在**同一条 SQL**；`qsearch` 用 `tools/embed_query.sh`（复用 Jina 模型）把查询文本转向量。
- 已实测：redis 10658 向量，`qsearch` 与外挂引擎结果一致。

**统一入口**：`search` 默认走 pgvector —— 嵌入查询后，向量候选来自 PG 内的 `$knn`，全文候选来自 `$fti`，RRF 在应用层融合；
`--vector-cmd` 才回退到外挂引擎。`qsearch` 是纯 pgvector KNN 的直查。两条向量路径不再割裂。

### 5.9 规范条目校验与受控写 Action

- **schema-as-data**：`/spec/{category}` 存规范 `{"required":[...],"types":{...}}`；`put` 与 `import-records --category` 写入前校验（违反抛错，批量则整批回滚）。
- **受控写**：`put`（校验 upsert）、`forget`/`restore`（归档/恢复，不物理删除）、`link`。
- **统一审计**：每个 Action 追加 `/audit/{agent}/{ts}-{pid}-{seq}`（`meta.action`/`target`），并建 `about` 边指向目标。
- 与 Agent 记忆 Action 共用同一套审计与归档语义。

## 6. 安全模型

| 风险 | 对策 |
|---|---|
| SQL 注入（值） | 所有值走 `$n` 参数绑定 |
| SQL 注入（JSONB 路径不可参数化） | 正则白名单 `^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*$` + 逐段校验，非法即拒绝 |
| 任意 SQL | AI 只能产出受限 AST；编译器只生成参数化 SELECT |
| 口径绕过 | `is_active` 在编译期注入，partial index 与之一致 |
| 谓词硬编码 | 谓词以 key 表示，编译期经 `knowledge` 表解析为 `predicate_id` |
| 远程写 | 写不开放 HTTP；局域网内 **SSH + 公钥** 执行本地 CLI（可 `command=`/`no-pty`/`from=` 限定）；DB 写角色仅 localhost |

---

## 7. 目录结构

```
Makefile              # 编译出单一二进制 knowledge
schema.sql            # DDL（PG16）
src/
  util.hpp            # 公共工具（COPY 转义 / relpath / UTF-8 截断）
  tokenize.hpp/.cpp   # 代码标识符分词器
  compile.hpp/.cpp    # JSE 校验 + 编译器（jansson + 参数化 SQL）
  api.hpp/.cpp        # search / context 复用实现（CLI + HTTP）
  server.hpp/.cpp     # HTTP API 服务（只读，连接池）
  vector.hpp/.cpp     # 外挂向量引擎 provider 调用
  importer.hpp/.cpp   # 代码知识导入器（chunks/call_graph/dataflow）
  books.hpp/.cpp      # 电子书导入器（/opt/books 的 Markdown）
  db.hpp/.cpp         # libpq 连接与查询
  main.cpp            # CLI（tokenize / compile / query / search / context / import / import-books / seed）
tools/                # ingest.sh（一键分析+入库）、vector_provider.sh、vector_provider_books.sh、vector_provider_stub.sh
tests/smoke.sh        # 回归测试（编译器 + PG 端到端 + provider 健壮性），make test
prompts/              # 向 Google AI 提问的三弹材料 + 统一 brief
README.md             # 快速上手
design.md             # 本文档
```

技术栈：**C++17 + libpq + jansson**，`make` 产出单文件二进制 `knowledge`（~150KB，动态链接系统库）。

---

## 8. 任务进度（Todolist）

### 已完成 ✅

- [x] **架构决策**：代理键、is_active 单轨、外挂向量、应用层分词、C++ 编译器
- [x] **环境**：安装 PostgreSQL 16，建 `knowledge` 库
- [x] **DDL**：`schema.sql`（双表 + 生成列 + 全部索引）并在 PG 落地
- [x] **JSE 校验 + 编译器**：`compile.cpp`，全部算子（`$and/$or/$not/$meta/$fti/$search/$triple/$k-hop` + 修饰符），路径白名单 + 全参数化绑定
- [x] **分词器**：`tokenize.cpp`（snake_case/camelCase 拆分）
- [x] **写入 helper**：`main.cpp` 内 `upsert_entry` / `link_triple`（upsert 条目 / link 三元组）
- [x] **C++ 实现**：`src/*.cpp` + `Makefile`，产出单文件二进制 `knowledge`（~150KB）
  - [x] 分词器 / JSE 编译器 / libpq 访问全部移植
  - [x] CLI：`tokenize` / `compile` / `query` / `search` / `context` / `seed`
  - [x] 4 类查询（全文 / k-hop / triple / 向量）在真实 PG 命中
  - [x] 安全：注入路径被拒、非法算子报错
  - [x] 安装 libpq-dev，依赖 jansson（与 my_db 一致）

- [x] **向量引擎接入**：`src/vector.cpp` provider 契约 + `tools/vector_provider.sh`（包装 `cache_query --analysis-dir`），`$search` / `search` / `context.related` 全部接通
- [x] **上下文供给**：`knowledge context` 返回 定义 + callers/callees + 调用路径（节点序列）+ 语义近邻
- [x] **`knowledge search`**：语义搜索 + PG 标量过滤融合出口

- [x] **真实导入器**：`src/importer.cpp`，`knowledge import --analysis-dir --project --root`
      - chunks_meta.jsonl → 条目；call_graph.json → `calls` 三元组；dataflow.json → `defines/assigns/uses` 三元组
      - 缺失端点自动补 stub 条目（func / variable），保证边不被 JOIN 丢弃
      - COPY staging + 单事务 + `ON CONFLICT` 落盘；redis 全量 8434 条目 + 3.6 万三元组，2.3s
      - key 归一 `/code/local/{project}/{relpath}/{symbol}`，与 provider 完全对齐（已端到端验证）

- [x] **一键流水线**：`tools/ingest.sh <repo>` = 分析（index/vector/hnsw）+ 入库；`--skip-analyze` 仅导入
- [x] **token 友好输出**：`meta`/`content` 返回嵌套对象（不再双重转义）；`search` 默认回 `key/symbol/file/line/signature/score`；`context` 支持 `--no-content` / `--max-code-bytes`
- [x] **电子书输入源**：`knowledge import-books`，`/opt/books/{book}/chapters/**/page_*.md` → book/chapter/page 条目 + `contains` 关系；`tools/vector_provider_books.sh` 接入 book 向量（key 与 `cache_query` 的 `name` 天然对齐）
- [x] **代码复盘加固**：修复 `$search_score` 无 `$search` 的非法 SQL；拒绝一个节点含多个算子；`$triple` 增加 `direction`（out/in/both）且语义修正；provider 容忍杂音输出；公共工具抽到 `src/util.hpp`；书籍正文不再整页写入 `search_tsv`（截断 4KB）
- [x] **回归测试**：`tests/smoke.sh` + `make test`（编译器 + PG 端到端 + provider 健壮性，16 项）
- [x] **编译期 key→id 解析**：`CompileOptions::resolve_key`，已知 key 直接产出整数 id 字面量（省去每个条件的子查询），未命中回退子查询
- [x] **`$k-hop` 结果上限**：递归子查询尾部 `LIMIT 20000`，防扇出爆炸
- [x] **混合检索（RRF）**：`search` = 向量候选 + 全文候选（`$fti` + `$fti_rank`），RRF 融合后过标量过滤；结果带 `rrf/vector_rank/lexical_rank/vector_score`
- [x] **导入流式化**：`JsonObjectStream` 逐条解析顶层对象，`call_graph.json`/`dataflow.json` 不再整文件载入内存；python 全量（11M call_graph / 18M dataflow）→ 55402 条目 / 132441 边 / 12.9s，结果与旧实现一致
- [x] **导入性能优化**（Linux 7.1.2：1,315,417 条目 / 1,569,588 边）
      - 内存：`name2keys` 只存调用图用到的名字、`known_keys`/`seen_edges` 改用 64 位哈希 → 峰值 RSS 271MB
      - DB 载入：`array_to_tsvector`（免二次解析）替代 `to_tsvector`
      - 大项目（>20 万行）自动「丢索引/FK → 批量重建」，小项目保持增量（避免固定重建开销）
      - 结果：290s → 200s（chunks 67s / knowledge insert 67s / statement 15s / 索引重建 39s）
- [x] **查询算子扩充（分析型）**：`$key`、`$group_by`、`$count`、度数排序 `$in_degree`/`$out_degree`、`$meta` 的 `$like/$ilike/$prefix`。已可表达"调用者最多的函数""文件函数数分布"等分析查询
- [x] **Linux 数据对齐**：用当前 `call_graph` 重建 linux70 图（54M / 219582 函数，563410 边），图与向量同源
- [x] **文档重新定位**：从"code search 系统"改为"可查询知识底座（code search 为首个应用）"
- [x] **search 加权 RRF**：向量 1.0 / 全文 0.5（修复纯语义查询被全文噪声并列）
- [x] **HTTP API（只读）**：`knowledge serve`（cpp-httplib + 连接池），端点 `/health` `/stats` `/jse` `/search` `/context`；服务端用 `knowledge_ro` 只读角色，写路径仅本地；请求体/超时/深度限额；`tests/smoke.sh` 覆盖

- [x] **Agent 记忆（Action 层，本地）**：`remember`/`fact`/`link`/`forget`；情景事件 + 键值事实；覆盖即归档（`/@archive` + `/pred/supersedes`）；`/mem/{agent}` 命名空间；provenance 自动注入
- [x] **口径注入修复**：查询默认 `AND is_active`（此前只是文档承诺未实现），`$include_archived` 可关闭
- [x] **调用图方向修复（重要）**：my_db `call_graph.json` 顶层键是 **callee**、`calls[].function` 是 **caller**（`file` 为 caller 文件）；此前 importer 读反了方向且把 caller 文件安到 callee 上。已修正为 `caller(精确自 file+name) -calls→ callee(按名解析)`。所有项目（redis/python/linux/linux70）重导并逐条对照源码验证（`__handle_mm_fault→handle_pte_fault→do_swap_page`、`hnsw_vectors_distance_bin→hnsw_popcount`）
- [x] **调用图精度（歧义名跳过）**：callee 名（JSON 顶层，无文件）歧义时默认跳过，`--fanout` 可开启；Linux 7.1.2 边 157 万 → 90 万，linux70 56.3 万 → 38.7 万
- [x] **context `--exclude-headers`**：过滤 `include/` 下的调用者/被调者（上游调用图把头文件宏误记为 caller，Linux 假边多源于此）；`do_swap_page` 的调用者过滤后全为真实 mm 函数
- [x] **语料导出**：`export --preset edges|context` → 训练用 JSONL（带 provenance 与关系）
- [x] **通用实体导入**：`import-records`（JSONL/CSV → 条目）
- [x] **向量进 PG（pgvector）**：`embedding VECTOR(768)` + HNSW；`import-vectors`（含 bin/meta 名称校验）；`$knn` 算子（距离为主序）；`tools/embed_query.sh` + `qsearch`
- [x] **检索统一**：`search` 默认 pgvector（`$knn` + `$fti` RRF），`--vector-cmd` 回退外挂；`source=pgvector+fti` / `vector+fti`
- [x] **规范条目校验（schema-as-data）**：`spec-set` 定义 `/spec/{category}`；`put` / `import-records --category` 写入前校验，批量违反整批回滚
- [x] **Action 全面化**：统一审计（`/audit/{agent}/...` + `about` 边）；通用受控写 `put`；`forget`/`restore`（归档/恢复）；`link` 纳入审计
- [x] **Makefile 依赖跟踪**：`-MMD -MP` + `-include *.d`（此前改头文件不重编导致链接错误）

### 进行中 🚧

- [ ] **多项目批量导入**：16 个仓库（约 250 万 chunk），分项目串行导入 + 校验

### 待办 ⏳

- [ ] **迁移对账（第三弹 E14）**：mmap 旧系统 vs PG 新系统的双跑 diff 与灰度切流
- [ ] **规范条目（第三弹 F17/F18）**：`meta.category=spec` 定义字段约束，写入校验与编译器共享路径白名单
- [ ] **LLM 报错闭环（第三弹 H24）**：语法/安全/规范三层结构化 Error Payload + System Prompt
- [ ] **Action 层（写侧）**：声明式动作 + 前置校验 + 归档/snapshot + 乐观锁（`version`）
- [ ] **Agent 插件**：宿主中立的 JSE 工具封装（DSH / Claude Code / opencode）
- [ ] **触发第三弹提问**：迁移对账、规范校验、LLM 报错

### 阻塞 / 依赖

- **真实导入器**是当前唯一关键路径：需把 `my_db` 产出（`chunks_meta.jsonl` / `call_graph.json` / `dataflow.json`）归一入库，并让 key 与向量 provider 的 `/code/local/{project}/{relpath}/{symbol}` 对齐。
- 导入完成前，`search` / `context.related` 只能命中 `seed` 的演示数据。
- 向量引擎已跑通（`cache_query --analysis-dir`），无阻塞。

---

## 9. 关键决策记录（ADR 摘要）

| # | 决策 | 理由 | 否决项 |
|---|---|---|---|
| 1 | 三元组端点用 BIGINT 代理键 | 千万级下索引缩小 60-70%、整数比较快 2-5x | TEXT key 易超长、索引膨胀、重构需级联 |
| 2 | `is_active` 生成列（不含时间函数） | IMMUTABLE 才能做生成列；谓词与索引一致 | 含 `now()` 的生成列在 PG 非法 |
| 3 | `key` 全表唯一（非 partial） | FK 引用目标 + 逻辑身份 | partial unique 不能作 FK 目标 |
| 4 | 向量：外挂 provider 与 pgvector **并存** | 外挂零导入；pgvector 让向量与标量/图同一条 SQL、同事务 | 单一方案都被否决 |
| 5 | 全文应用层预分词 + `search_tsv` | 代码标识符默认分词器命中差；通用可移植 | pg_search/BM25 引入第三方扩展 |
| 6 | 编译器用 C++17 + libpq + jansson | 可编译为单文件二进制、与 my_db 工具链一致、无运行时依赖 | TypeScript（需 Node/Bun 运行时）、Go |
| 7 | `$k-hop` 用递归 CTE（ID 拓扑） | 免引入图数据库；写侧实时更新友好 | ltree（树限制，无法表达 DAG/环）、物化闭包表（维护成本高） |

---

## 10. 参考

- 原始路线分析：`prompts/context-brief.md`
- 提问材料：`prompts/strike-1.md`、`strike-2.md`、`strike-3.md`
- 快速上手：`README.md`

---

## 11. 修复记录（Bugfix Log）

| 日期 | 级别 | 问题 | 修复 |
|---|---|---|---|
| 2026-09-29 | **严重** | 调用图**方向反了**：把 my_db `call_graph.json` 的顶层键（callee）当成 caller、`calls[].function`（caller）当成 callee，且把 caller 文件安到 callee 上 | 修正为 `caller(精确 file+name) -calls→ callee(按名解析)`；redis/python/linux/linux70 全部重导并对照源码验证 |
| 2026-09-29 | 高 | 同名 callee（无文件）fan-out 造出大量假边 | 分歧名默认跳过，`--fanout` 可开启 |
| 2026-09-29 | 中 | `$meta` 的 `$in`/`$nin` 数值分支：text 与 `numeric[]` 比较直接报错 | 数值时路径 `::numeric` |
| 2026-09-29 | 中 | 查询**未注入** `AND is_active`（此前只是文档承诺） | 默认注入，`$include_archived` 关闭 |
| 2026-09-29 | 中 | `$k-hop` 到达节点未校验 `is_active` | 内层加 `k.is_active` |
| 2026-09-29 | 中 | 一个节点含多个算子被静默取第一个 | 强制"恰好一个算子" |
| 2026-09-29 | 中 | `$knn` 与 `$order` 同现时距离排序被覆盖 | 距离恒为主序，`$order` 次之 |
| 2026-09-29 | 低 | 记忆事件 key 跨进程同毫秒碰撞 | key 加入 pid |
| 2026-09-29 | 低 | `import-vectors` 可能静默错位 | 校验 `.bin` 内 name 与 meta 一致，不符则跳过并告警 |
| 2026-09-29 | 低 | `import-vectors` 更新时重复 key 不确定 | `DISTINCT ON (key)` |
| 2026-09-29 | 低 | CSV 导入 CRLF 末列带 `\r` | 逐行 strip CR |
| 2026-09-29 | 低 | Makefile 不跟踪头文件依赖，改头文件不重编导致链接错误 | `-MMD -MP` + `-include *.d` |
| 2026-09-29 | 低 | `$search_score` / `$fti_rank` 缺前置算子时生成非法 SQL | 编译期显式报错 |
