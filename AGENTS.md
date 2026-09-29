# AGENTS.md

给 AI Agent 的项目速查：**Knowledge** 是一个可查询的知识底座（code search 是其第一个应用）。
把代码、电子书、任意结构化记录、Agent 记忆统一存为「条目 + 三元组」，用受控的 **JSE** 查询语言访问。

- 单个二进制 `./knowledge`（`make` 编译），源码在 `src/`。
- 状态全在 PostgreSQL；查询层无状态，可多调用方共享（只读 HTTP API）。
- **写只在本地**（或局域网 SSH+key）；远程 API 只读。

---

## 1. 环境与构建

```bash
sudo apt-get install -y libpq-dev libjansson-dev postgresql-16-pgvector
make
export DATABASE_URL=postgres://knowledge:knowledge@127.0.0.1:5432/knowledge   # 默认即可
```

查询文本→向量（语义检索用）：`tools/embed_query.sh`（复用 my_db 的 Jina 模型，需 GPU）。
外挂向量回退：`tools/vector_provider.sh`（代码）、`tools/vector_provider_books.sh`（电子书）。

```bash
export KNOWLEDGE_EMBED_CMD=./tools/embed_query.sh          # search 默认用它
export KNOWLEDGE_VECTOR_CMD=./tools/vector_provider.sh      # 外挂引擎（可选）
```

---

## 2. 命令总览

| 命令 | 用途 | 读写 |
|---|---|---|
| `query` | 执行 JSE（从 stdin 读 JSON），返回行 | 读 |
| `compile` | 只编译 JSE → SQL（不连库执行，调试用） | - |
| `search "<NL>"` | 混合检索：向量(pgvector) + 全文 RRF | 读 |
| `qsearch "<NL>"` | 纯 pgvector KNN | 读 |
| `context <key>` | 一个节点的上下文包（定义+callers+callees+paths+related） | 读 |
| `tokenize <text>` | 代码标识符分词（调试用） | - |
| `serve` | 只读 HTTP API（`/health /stats /jse /search /context`） | 读 |
| `put` | 受控写：校验后 upsert 条目 | 写（本地） |
| `remember` / `fact` | Agent 记忆（情景事件 / 键值事实） | 写（本地） |
| `link` / `forget` / `restore` | 建关系 / 归档 / 恢复 | 写（本地） |
| `spec-set` | 定义某类条目的规范（schema-as-data） | 写（本地） |
| `export` | 导出训练语料 JSONL（`--preset edges\|context`） | 读 |
| `import` `import-books` `import-records` `import-vectors` | 导入数据 | 写（本地） |

---

## 3. 检索：先从自然语言到 key，再取上下文（省 token）

```bash
# 1) 语义+全文混合，拿到候选 key（默认 pgvector，source=pgvector+fti）
./knowledge search "memory pool allocation" --k 5
#   -> [{ key, meta.symbol, meta.file, meta.line, meta.signature, rrf, vector_rank, lexical_rank }]

# 2) 对目标 key 取上下文（一次拿全，可裁剪）
./knowledge context /code/local/redis/src/module.c/RM_PoolAlloc --depth 2 \
  --predicate /pred/calls --no-content          # --no-content / --max-code-bytes N 省 token
```

`context` 返回：

```json
{ "definition": [...], "callers": [...], "callees": [...],
  "paths": [{"depth":2,"keys":"{A,B,C}"}], "related": [{"key","meta","score"}] }
```

- `--exclude-headers`：过滤 `include/` 下的调用者/被调者（Linux 假边多源于头文件宏）。

---

## 4. JSE 查询语言

AI 只产出**受限 JSON 表达式**，编译器翻译为参数化 SQL（安全、可审计）。

```json
{ "$where": { "$and": [
    { "$key": { "$prefix": "/code/local/linux/mm/" } },
    { "$meta": { "path": "kind", "$eq": "function" } },
    { "$fti": "alloc" },
    { "$triple": { "predicate": "/pred/calls", "direction": "in" } }
  ]},
  "$project": ["key","meta.symbol","meta.file"],
  "$order": { "$in_degree": "desc" },
  "$limit": 20,
  "$count": false
}
```

### 算子

| 类别 | 算子 |
|---|---|
| 逻辑 | `$and` `$or` `$not`（每节点恰好一个算子） |
| 属性 | `$meta`: `path` + `$eq/$ne/$gt/$gte/$lt/$lte/$in/$nin/$exists/$like/$ilike/$prefix` |
| 键 | `$key`：对条目 `key` 施加同组运算符（按项目/路径前缀过滤） |
| 全文 | `$fti`：应用层分词的全文检索（`search_tsv @@ to_tsquery`） |
| 向量(外挂) | `$search`：需外部向量候选（`query --vectors <json>` 或 `search --vector-cmd`） |
| 向量(PG) | `$knn`: `{"vector":[...],"k":N}` → `ORDER BY embedding <=> $vec` |
| 关系 | `$triple`: `subject`/`predicate`/`object` + `direction`(`out`/`in`/`both`) |
| 图遍历 | `$k-hop`: `from`/`predicates`/`depth`(1-5)/`direction`/`where`，ID 拓扑 + path 防环 |

### 修饰符

- `$project`（字段裁剪）、`$order`、`$limit`（≤1000）、`$offset`（≤1000000）
- `$count: true`（匹配总数）、`$group_by: "<path>"`（分组计数，按 count 降序）
- **`$count` / `$group_by` 忽略 `$order`**（聚合后行已重排）
- `$order` 排序键：`$search_score`（需 `$search`）、`$fti_rank`（需 `$fti`）、`$in_degree` / `$out_degree`（边度数）
- `$include_archived: true`：默认只查活跃数据（`is_active`）；归档需显式包含

### 路径与类型约定

- `$meta.path` 相对 `meta` 根（`lang` → `meta->>'lang'`）
- `$project` / `$order` 从行根起算（`key`、`meta.symbol`、`content.code`、`content.text`）
- `$project` 输出**原生 JSON 类型**（`meta.line` 是数字、`content.text` 是字符串）；
  行根 `id`/`version` 也是数字
- 数值比较带守卫：右值为数值时左值须是数字文本才比较（文本列不再报转换错）
- `$prefix` 是**字面量**（`%`/`_` 已转义）；`$knn` 只能在最外层且只能一个，可投影 `$knn_distance`

---

## 5. Key 与数据约定

| 前缀 | 含义 | 关键 meta |
|---|---|---|
| `/code/local/{project}/{relpath}/{symbol}` | 代码条目（函数/结构体/宏/头/文件） | `project,kind,lang,file,line,symbol,signature` |
| `/books/{book}/chapters/{chapter}/page_NNNN` | 电子书页 | `kind,book,chapter,page,file` |
| `/data/{...}` | `import-records` 写入的任意记录 | `kind,source,<标量字段>` |
| `/mem/{agent}/events|facts/...` | Agent 记忆 | `kind,agent,session,ts,tags,topic` |
| `/spec/{category}` | 规范条目（schema-as-data） | `category,applies_to` |
| `/audit/{agent}/{ts}` | 写操作审计 | `kind,agent,action,target,ts` |
| `/pred/{name}` | 谓词定义（自身也是条目） | `category=predicate` |

常用谓词：`/pred/calls`、`/pred/uses|defines|assigns`（数据流）、`/pred/contains`（书：book→chapter→page）、`/pred/about`（审计/事件→目标）、`/pred/supersedes`（事实覆盖）。

---

## 6. 典型工作流

**A. 用自然语言找代码并理解**
```bash
./knowledge search "slab allocator" --k 5
./knowledge context <key> --depth 2 --exclude-headers
```

**B. 分析型探索（原生 code search 做不到）**
```bash
# 某目录下被调用最多的函数
echo '{"$where":{"$and":[{"$key":{"$prefix":"/code/local/linux/mm/"}},
  {"$meta":{"path":"kind","$eq":"function"}}]},"$project":["meta.symbol"],
  "$order":{"$in_degree":"desc"},"$limit":10}' | ./knowledge query
# 分组计数
echo '{"$where":{"$key":{"$prefix":"/code/local/linux/"}},"$group_by":"meta.kind"}' | ./knowledge query
```

**C. 谁调用了某函数 / 调用链**
```bash
./knowledge context <key> --depth 2 --predicate /pred/calls --no-content
# 或 JSE: {"$triple":{"object":"<key>","predicate":"/pred/calls","direction":"in"}}
```

**D. 跨源 / 跨项目**
- 不写 `$key` 前缀即可跨代码/电子书/记录一起检索（同一张图，可 join）。
- 代码条目带 `meta.project`，可按项目聚合对比：
```bash
# 各项目的分配类函数数量
echo '{"$where":{"$and":[{"$key":{"$prefix":"/code/local/"}},
  {"$meta":{"path":"kind","$eq":"function"}},
  {"$meta":{"path":"symbol","$ilike":"%alloc%"}}]},"$group_by":"meta.project"}' | ./knowledge query
```

**E. 导出训练语料（JSONL）**
```bash
./knowledge export --preset edges   --predicate /pred/calls --key-prefix /code/local/linux/ > calls.jsonl
./knowledge export --preset context --key-prefix /code/local/linux/mm/ --depth 1 --no-content > ctx.jsonl
```

**F. 作为记忆（本地写）**
```bash
./knowledge remember --agent me --text "结论：..." --about <key> --tag note
./knowledge fact     --agent me --topic preferred_db --value '"postgresql"'   # 覆盖即归档旧值
```

---

## 7. 受控写（本地 Action）

```bash
./knowledge spec-set note '{"required":["meta.title","content.text"]}'
./knowledge put --key /data/note/1 --meta '{"title":"T"}' --content '{"text":"..."}' --category note
./knowledge forget <key>        # 归档（不物理删除）
./knowledge restore <key>
```

- 所有写操作追加 `/audit/{agent}/...`，带 `meta.action`/`target` 并建 `about` 边。
- `put`/`import-records --category` 按 `/spec/{category}` 校验；批量违反整批回滚。
- **version 内容变化才 +1**（幂等重写 version 不动）；`link`/`remember --about` 端点不存在会报错回滚。
- **写即复活**：`put`/导入到已归档条目会恢复其活跃状态（免先 `restore`）。

---

## 8. 只读 HTTP API

```bash
./knowledge serve --port 8931 --ro        # 只读角色，DB 层禁止写
# GET  /health  /stats  /search?q=&k=  /context?key=&depth=&exclude_headers=
# POST /jse     (body 为 JSE；可带 "$vectors":[...] 供 $search)
```

限额：请求体 ≤1MB、`statement_timeout=30s`、`$limit ≤1000`、`$k-hop depth ≤5`；
`search k` 钳制 1..1000、`context depth` 钳制 0..5；非法 JSE 返回 400 + `[JSE] ...`。

---

## 9. Agent 注意事项（易错点）

1. **默认只读活跃数据**：归档条目默认不返回（用 `$include_archived:true` 查看历史）。
2. **`$meta.path` 相对 meta，`$project` 从行根起算**——两套路径约定。
3. **`$order $search_score` 需 `$search`；`$fti_rank` 需 `$fti`；`$knn` 距离恒为主序**。
4. **每节点恰好一个算子**，多算子会被拒绝。
5. **图可能有名歧义噪声**：callee 按名解析（同名多文件默认跳过）；`context --exclude-headers` 去头文件宏噪声。
6. **检索要用"key 优先"两段式**：先 `search` 拿 key，再 `context` 取定义/关系，避免把大段源码塞进上下文。
7. **写只在本机**（或 SSH+key）；远程用只读 API。
8. `search` 需要向量：默认走 `KNOWLEDGE_EMBED_CMD`；无 GPU 时用 `--vector-cmd` 指外挂引擎，或退回纯全文。

---

## 10. 快速自检

```bash
make test          # 编译器/查询/记忆/API 冒烟（63 项）
make debug         # -O0 调试构建；make asan → ASan/UBSan 构建
./knowledge tokenize "__alloc_pages_slowpath"     # 应拆出 alloc/pages/slowpath
./knowledge version
```
