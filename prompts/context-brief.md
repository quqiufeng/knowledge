# 系统上下文（Context Brief）

> 每次新开 AI 对话时，**先完整粘贴本文件内容**，再粘贴对应弹次的问题。
> 三弹共用同一份 brief，不要删减。
> v2：第一弹答案已锁定代理键与 is_active 单轨，schema 已更新。

【系统上下文（System Context Brief）】
我们正在构建一个名为 Knowledge 的 AI Agent 本体（Ontology）知识库基础设施。

1. 当前落地场景与规模（代码知识库域）：
- 目标数据：16 个开源代码仓库 ≈ 250 万条代码 Chunk 条目，768 维向量（约 7.7GB），代码调用关系约 19.8 万节点 / 数百万条 calls / implements 三元组。
- 数据映射：content 为源码段，meta 结构为 {"kind": "function", "lang": "c", "file": "mm/page_alloc.c", "line": 312, "symbol": "__alloc_pages_slowpath"}。

2. 数据库 Schema（PostgreSQL 15+，v2 —— 已采纳 BIGINT 代理键与 is_active 单轨生成列）：

```sql
-- 信息条目表（存储代码 chunk、类/函数节点、规范文件、谓词定义等一切节点）
CREATE TABLE knowledge (
    id BIGSERIAL PRIMARY KEY,
    key TEXT UNIQUE NOT NULL, -- 全表唯一（FK 引用目标 + 逻辑身份），如 "/code/local/linux/mm/page_alloc.c/__alloc_pages_slowpath"
    meta JSONB NOT NULL DEFAULT '{}'::jsonb,
    content JSONB NOT NULL DEFAULT '{}'::jsonb,
    version BIGINT NOT NULL DEFAULT 0,
    start_time TIMESTAMPTZ,
    end_time TIMESTAMPTZ,
    is_archived BOOLEAN NOT NULL DEFAULT FALSE,
    -- 单轨活跃状态：IMMUTABLE（不含时间函数），STORED 生成列可用
    is_active BOOLEAN NOT NULL
        GENERATED ALWAYS AS (NOT is_archived AND end_time IS NULL) STORED,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- 三元组关系表（存储调用链、继承链等连线；端点为 BIGINT 代理键）
CREATE TABLE statement (
    id BIGSERIAL PRIMARY KEY,
    subject_id BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    predicate_id BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    object_id BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    meta JSONB NOT NULL DEFAULT '{}'::jsonb,
    start_time TIMESTAMPTZ,
    end_time TIMESTAMPTZ,
    is_archived BOOLEAN NOT NULL DEFAULT FALSE,
    is_active BOOLEAN NOT NULL
        GENERATED ALWAYS AS (NOT is_archived AND end_time IS NULL) STORED,
    CONSTRAINT uq_stmt UNIQUE NULLS NOT DISTINCT (subject_id, predicate_id, object_id, start_time)
);

-- 图遍历热路径索引（partial 谓词与编译器注入谓词逐字一致）
CREATE INDEX idx_stmt_traversal
    ON statement (subject_id, predicate_id, object_id)
    WHERE is_active;
```

3. JSE (JSON S-Expression) 范例：
AI Agent 不写裸 SQL，而是输出带有过滤、图展开、投影、排序与分页的 AST 表达式，由应用层安全编译为参数化 SQL。
路径约定：`$meta` 算子的 `path` 相对于 meta 列根节点（如 `lang` 即 `meta->>'lang'`），而 `$project` 路径从行根节点起算（如 `meta.symbol`）。
谓词约定：JSE 中谓词以 key 字符串表示（如 `"calls"`），编译期解析为 `predicate_id`（查 knowledge 表 key→id，带缓存）。

```json
{
  "$and": [
    { "$meta": { "path": "lang", "$eq": "c" } },
    { "$fti": "page_alloc" },
    {
      "$k-hop": {
        "depth": 2,
        "predicates": ["calls"],
        "where": { "$meta": { "path": "kind", "$eq": "function" } }
      }
    }
  ],
  "$project": ["key", "meta.symbol", "meta.file"],
  "$order": { "$search_score": "desc" },
  "$limit": 20
}
```

4. 已定决策（供参考，不必重新论证）：
- 编译器技术栈：TypeScript（Node.js/Bun），AST 用 discriminated unions + zod/valibot 校验。
- 三元组端点用 BIGINT 代理键；导入走 staging 临时表 COPY → ID 转换 → ON CONFLICT 落盘。
- 图遍历用递归 CTE（仅传整型 ID + path 防环 + 深度硬限 + 遍历后 Late Binding JOIN 属性）。
- 状态单轨：`is_active` 生成列，编译器注入 `AND is_active`，与 partial index 谓词逐字一致。
- 向量走外挂引擎（沿用 .hnsw 文件，不入 PG）；`$search` 编译为候选 key 的 VALUES CTE。
- 全文走应用层预分词（snake_case/camelCase 拆分）写入 `search_tsv` + GIN 索引，查询 `to_tsquery('simple', ...)`。
- 本项目为只读查询面起步（Action 层后续演进），写入来自受控索引管线。
