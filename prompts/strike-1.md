# 第一弹：存储层、遍历与编译器安全

> 用法：先贴 `context-brief.md` 全文，再贴本文件横线以下内容。

---

【核心架构与工程技术提问】

结合上述上下文，请针对我们系统当前面临的 4 个关键工程卡点，给出 PostgreSQL 15+ 环境下的具体代码、SQL 或架构级深度解法：

## 1.（存储层与千万级性能）三元组端点类型选择与千万级数据导入痛点

目前 `statement` 的 `subject/predicate/object` 使用 TEXT 类型的 `key` 并且加了外键参照 `knowledge(key)`。但在千万级三元组下，长字符串（如代码文件路径）会导致 B-Tree 索引体积暴涨、Join 性能骤降。

- 方案对比：使用 `TEXT key + FK` vs `BIGINT` 代理键（外键关联 `knowledge.id`），在千万级三元组下的索引体积、Join 速度、调试成本与可读性上如何取舍？
- 导入开销：在海量代码解析结果批量导入时，`statement` 表上的 `UNIQUE NULLS NOT DISTINCT` 约束和外键校验会极大地拖慢 `COPY` 写入性能。当前 FK 已声明为 `DEFERRABLE INITIALLY DEFERRED`：请说明它对锁持有时间与异常中断的影响，以及批量导入场景下如何利用与规避（如禁用 Trigger、Unlogged 临时表 + Distinct 写入等手段的取舍）。

## 2.（存储层与图遍历）递归 CTE 千万级多跳遍历的性能突破

在现有的 `statement` 表上，使用递归 CTE 进行多跳函数调用链展开（如 `calls` 关系 2~3 跳）且带中间节点条件过滤时，容易出现全表扫描或爆内存。

- 在不引入独立图数据库（如 Neo4j）的前提下，基于 PostgreSQL 扩展（如 `ltree` 路径树）或增量物化闭包表（Closure Table），如何实现"高频函数调用图展开"与"实时增量写入/修改三元组"的最佳平衡？
- 注意：调用图是 DAG 且可能含环（函数互递归），`ltree` 的单父节点树限制如何应对？请一并给出递归 CTE 的防爆炸手段（深度上限、中间结果行数上限，而非仅靠外层 LIMIT 截断）。

## 3.（存储层与过滤优化）状态双轨制与 B-Tree 范围索引优化

系统存在 `is_archived` 布尔值与 `start_time/end_time` 生效区间的"状态双轨制"（查询时需默认注入 `WHERE is_archived = FALSE AND (end_time IS NULL OR end_time > NOW())`）。

- 架构评估：将"是否归档"与"生效区间"双轨表达，在编译期注入时容易造成谓词冗余并破坏索引。是否建议在数据库层面将其合并为单轨（例如通过生成列或触发器维护单一的 `is_active` 状态）？
- 索引设计：对于大量的历史代码条目，`end_time IS NULL` 属于长尾高频值。针对这种场景，如何设计 Partial Index（条件索引）或生成列，以维持最高的查询选择性？

## 4.（编译器与绝对安全）JSONB 路径提取的参数化防御与 SQL 注入防范

在 JSE 解析中，谓词值（如 `"page_alloc"`）可以通过 SQL 参数绑定（`$1`）解决，但 JSONB 的字段路径提取标识符（如 `$meta.path = "properties.grade"` 中的字段路径）无法使用标准 `$1` 绑定。

- 请提供一套 TypeScript 的编译器实现逻辑（我们技术栈已定为 TS + zod/valibot），说明如何通过 AST 语法树解析、路径白名单校验与安全转义机制，确保 JSONB 路径提取过程达到绝对防御 SQL 注入的标准。
