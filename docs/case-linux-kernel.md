# 案例：用 Knowledge 完整探索 Linux 内核

本文是一次真实的端到端探索记录：用 **Knowledge**（条目 + 三元组 + JSE）遍历 Linux 内核，
从全局概览 → 定位子系统 → 理解单个函数 → 分析型问题 → 调用链/数据流 → 跨源检索。
所有数字与结果均为实跑输出（数据经过调用图方向修正与精度清洗）。

---

## 0. 数据与准备

| 数据源 | 规模 | 具备 |
|---|---|---|
| `linux`（7.1.2） | 1,315,419 条目 / 902,662 条 `calls` 边 | 全文 + 调用图 |
| `linux70`（7.0.11） | 465,801 条目 / 386,790 条边 | 全文 + 调用图 + 数据流 + 向量 |

```bash
export DATABASE_URL=postgres://knowledge:knowledge@127.0.0.1:5432/knowledge
export KNOWLEDGE_EMBED_CMD=./tools/embed_query.sh        # 语义检索用（Jina，GPU）
```

---

## 1. 全局概览：先看家底

**各类符号分布**（`$group_by`，原生 code search 给不了）：

```bash
echo '{"$where":{"$key":{"$prefix":"/code/local/linux/"}},
       "$group_by":"meta.kind","$limit":5}' | ./knowledge query
```
```
function 704259 · header 371489 · struct 113114 · file 58414 · enum 41584
```

**函数最多的文件**（定位"重头"模块）：

```
drivers/accel/rocket/rocket_registers.h     655
drivers/infiniband/hw/hfi1/chip.c           601
kernel/bpf/verifier.c                       549
drivers/net/ethernet/broadcom/bnxt/bnxt.c   543
kernel/events/core.c                        536
```

> 洞察：`bpf/verifier.c`（549）、`kernel/events/core.c`（536）是核心逻辑密集区；
> 头文件 `rocket_registers.h` 函数多，是寄存器宏展开的表征。

---

## 2. 定位子系统：自然语言 → key

```bash
./knowledge search "slab allocator memory pool" --k 5
```
```
___slab_alloc        mm/slub.c
allocate_slab        mm/slub.c
alloc_from_new_slab  mm/slub.c
...
```

拿到候选 `key` 后再精确取上下文——**两段式**，不把大段源码塞进上下文。

---

## 3. 理解一个函数：上下文包

以页表缺页核心 `handle_pte_fault` 为例：

```bash
./knowledge context /code/local/linux/mm/memory.c/handle_pte_fault \
  --depth 1 --predicate /pred/calls --no-content
```
```
callers: ['__handle_mm_fault']
callees: ['vma_is_accessible','do_numa_page','do_pte_missing',
          'do_swap_page','do_wp_page','__pte_alloc','pte_offset_map_rw_nolock']
```

> 一次调用同时拿到：定义（signature）、直接调用者、被调者、路径、语义近邻。
> 与内核源码一致：`__handle_mm_fault → handle_pte_fault → do_swap_page/…`

---

## 4. 分析型探索（原生 code search 做不到）

**某子系统里被调用最多的函数**（按边度数排序）：

```bash
echo '{"$where":{"$and":[{"$key":{"$prefix":"/code/local/linux/"}},
   {"$meta":{"path":"file","$prefix":"mm/"}},{"$meta":{"path":"kind","$eq":"function"}}]},
   "$project":["meta.symbol"],"$order":{"$in_degree":"desc"},"$limit":6}' | ./knowledge query
```
```
kstrdup · memdup_user · memdup_user_nul · kstrndup · kfree · filemap_write_and_wait_range
```

> mm/ 里被复用最多的是通用分配/拷贝原语（`kstrdup`/`kfree`/`memdup_user`）——一目了然。

**组合查询：fs/ 下含 "lock" 且有调用者的函数数**

```bash
echo '{"$where":{"$and":[{"$key":{"$prefix":"/code/local/linux/fs/"}},
   {"$meta":{"path":"kind","$eq":"function"}},{"$fti":"lock"},
   {"$triple":{"predicate":"/pred/calls","direction":"in"}}]},"$count":true}' | ./knowledge query
```
```
5908
```

> 一条 JSE 同时完成：路径前缀 + 类型过滤 + 全文匹配 + 图约束 + 计数。
> 原生要 search 后再逐条查调用者，无法一条表达。

---

## 5. 调用链与影响面

**从入口展开 2 跳调用路径**：

```bash
./knowledge context /code/local/linux/mm/memory.c/__handle_mm_fault \
  --depth 2 --predicate /pred/calls --no-content
```
```
1 跳: pmd_alloc, p4d_alloc, pmd_lock, vma_is_accessible, linear_page_index …
2 跳叶子 Top: vma_is_anonymous, do_huge_pmd_anonymous_page,
              softleaf_from_pmd, touch_pmd, mapping_gfp_mask …
```

**谁调用了 `schedule`（影响面）**：

```bash
echo '{"$where":{"$triple":{"object":"/code/local/linux/kernel/sched/core.c/schedule",
   "predicate":"/pred/calls","direction":"in"}},"$count":true}' | ./knowledge query
```
```
48            # 内核中有 48 个函数的调用图入口指向 schedule
```

---

## 6. 数据流（linux70）

**变量 `gfp` 被哪些函数使用**：

```bash
echo '{"$where":{"$triple":{"object":"/code/local/linux70/vars/gfp",
   "predicate":"/pred/uses","direction":"in"}},"$count":true}' | ./knowledge query
```
```
166           # 分配器相关的 166 个函数使用了 gfp
```

也可列出具体函数（`kmemleak_scan_area`、`fanotify_alloc_path_event`、`mas_dup_build`…）。

---

## 7. 跨源检索：代码 + 电子书同一张图

```bash
echo '{"$where":{"$fti":"scheduler"},"$project":["key"],"$limit":6}' | ./knowledge query
```
```
/code/local/redis/src/latency.c/createLatencyReport
/books/ddia/chapters/18-Chapter_10._Batch_Processing/page_0009
/books/ddia/chapters/22-Index/page_0000
...
```

> 不限定 `$key` 前缀，就能把**代码、电子书、任意记录**一起检索——它们在同一张图里，可 join。

---

## 8. 结论：与原生 code search 对照

| 任务 | 原生 code search | Knowledge |
|---|---|---|
| 自然语言找函数 | ✓ | ✓（持平） |
| 单函数 callers/callees | 有 `context`，但在本数据上返回空 | ✓ 正确（方向修正后） |
| 多跳调用链 / 影响面 | `--depth`（受限） | ✓ 任意深度/方向 + 路径序列 |
| **聚合统计**（kind 分布、文件函数数） | ✗ | ✓ `$group_by` |
| **度数排序**（谁被调用最多） | ✗ | ✓ `$in_degree` / `$out_degree` |
| **组合查询**（全文+元数据+图+计数） | ✗ | ✓ 一条 JSE |
| 数据流 | 有 `dataflow` 命令 | ✓ 三元组查询 |
| **跨源**（代码+书+多项目） | ✗ | ✓ |
| 只读 API / 多 Agent 共享 | ✗ | ✓ |

**一句话**：把 Linux 内核当成"可查询的代码图"后，Knowledge 在**分析型探索**（统计、排序、组合、跨源）上明显强于原生；
在**纯语义找函数**上与原生持平。代价是要先导入、维护 key 对齐；而写只在本地、读可共享。

---

## 附：可复现命令清单

```bash
make && make test
./knowledge search "slab allocator" --k 5
./knowledge context <key> --depth 2 --exclude-headers --no-content
echo '{"$where":{...},"$group_by":"meta.file"}' | ./knowledge query
echo '{"$where":{...},"$order":{"$in_degree":"desc"}}' | ./knowledge query
./knowledge qsearch "page cache writeback" --k 5
```
