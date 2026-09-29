# 第二弹：向量架构、全文分词与 Action 设计

> 用法：先贴 `context-brief.md` 全文，再贴本文件横线以下内容。

---

【第二弹：检索层选型、向量架构与 Action 设计】

结合上述上下文，请针对以下 3 个关键工程卡点给出 PostgreSQL 15+ 环境下的架构级与代码级深度解法：

## 1.（向量架构抉择）pgvector vs 双引擎（外挂 USearch/Faiss）深度对比

250 万条代码 Chunk（768 维向量约 7.7GB）在 pgvector 上规模本身不算极端；真正的工程取舍在于动态更新与融合架构。

- 请对比三套架构：① 纯 PostgreSQL + pgvector（HNSW）；② PG（标量）+ 外挂内存/磁盘向量引擎（如 USearch/Faiss），JSE 的 `$search` 算子编译到外挂引擎；③ pgvector + pg_search 组合（向量与 BM25 各自独立索引，RRF 在应用层或 SQL 层融合）。
- 核心考量点：代码仓库 git 更新触发重建索引时，pgvector HNSW 无原生 delete/replace，索引膨胀与重建策略怎么设计？外挂引擎方案下，结果集与 SQL 侧标量过滤、融合排序（RRF）在应用层做还是 SQL 层做，一致性如何保证？锁定 pgvector 的长期迁移成本如何评估？

## 2.（代码标识符分词）ts_rank vs 真 BM25（pg_search）的取舍

代码 Chunk 包含大量蛇形命名（`__alloc_pages_slowpath`）或驼峰命名（`AllocPageSlow`）。默认的 english 分词器无法命中 `page_alloc`。

- PostgreSQL 内置的 tsvector + ts_rank 在代码搜索场景下与真正的 BM25（如 pg_search / Elasticsearch）差距有多大？是否值得为了 BM25 引入 pg_search 扩展？
- 如果保留 PG 原生全文检索：如何配置分词器（自定义 text search dictionary、Regexp 模板、预分词入库、pg_trgm 前缀/模糊）才能高效支持 C/C++ 标识符的前缀与子串匹配？请给出"搜 `page_alloc` 能命中 `__alloc_pages_slowpath`"的具体可运行配置与召回/索引体积评估。

## 3.（动作层与状态机守卫）声明式 Action 架构设计

我们计划将代码库变更（如发布新版本代码、废弃旧接口、重新解析建立调用链）建模为受控的 Action。

- 请设计声明式 Action 的 Schema 结构：如何表达 Precondition 状态机守卫（如：只有 `meta.status = 'draft'` 的条目才能执行 `publish` 动作），并声明式定义其副作用（Side-effects，如写条目、构建 snapshot 三元组、更新 version 并归档旧版本）？
- Action 的 schema 本身存储为 knowledge 规范条目（schema-as-data）时，校验引擎如何加载与版本化？
- 这种声明式 Action 如何实现类似 Palantir Actions 的完全可审计、可重放与历史回溯能力？多 Agent 并发执行同一 Action 时的冲突处理（version 乐观锁重试 vs 串行化）如何设计？
