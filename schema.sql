-- Knowledge schema v2 (PostgreSQL 16)
-- 条目(节点) + 三元组(边) 双表；BIGINT 代理键；is_active 单轨生成列。
-- 向量走外挂引擎（不入库），全文检索用应用层预分词写入 search_tsv。

BEGIN;

CREATE EXTENSION IF NOT EXISTS vector;

-- ============================================================
-- 信息条目表：代码 chunk、类/函数节点、规范文件、谓词定义等一切节点
-- ============================================================
CREATE TABLE knowledge (
    id            BIGSERIAL PRIMARY KEY,
    key           TEXT UNIQUE NOT NULL,          -- 逻辑身份 + FK 引用目标（全表唯一）
    meta          JSONB NOT NULL DEFAULT '{}'::jsonb,   -- {kind,lang,file,line,symbol,...}
    content       JSONB NOT NULL DEFAULT '{}'::jsonb,   -- 代码/正文
    embedding     VECTOR(768),                   -- 语义向量（pgvector，可选）
    search_tsv    TSVECTOR,                      -- 应用层分词器预计算（simple config）
    version       BIGINT NOT NULL DEFAULT 0,     -- 乐观锁
    start_time    TIMESTAMPTZ,
    end_time      TIMESTAMPTZ,
    is_archived   BOOLEAN NOT NULL DEFAULT FALSE,
    is_active     BOOLEAN NOT NULL
                  GENERATED ALWAYS AS (NOT is_archived AND end_time IS NULL) STORED,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ============================================================
-- 三元组关系表：调用链、继承链等连线；端点为 BIGINT 代理键
-- ============================================================
CREATE TABLE statement (
    id            BIGSERIAL PRIMARY KEY,
    subject_id    BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    predicate_id  BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    object_id     BIGINT NOT NULL REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    meta          JSONB NOT NULL DEFAULT '{}'::jsonb,
    start_time    TIMESTAMPTZ,
    end_time      TIMESTAMPTZ,
    is_archived   BOOLEAN NOT NULL DEFAULT FALSE,
    is_active     BOOLEAN NOT NULL
                  GENERATED ALWAYS AS (NOT is_archived AND end_time IS NULL) STORED,
    CONSTRAINT uq_stmt UNIQUE NULLS NOT DISTINCT (subject_id, predicate_id, object_id, start_time)
);

-- ============================================================
-- 索引：热路径 partial index，谓词与 JSE 编译器注入的 `AND is_active` 逐字一致
-- ============================================================

-- 图遍历正向：subject -> object（calls / implements / contains）
CREATE INDEX idx_stmt_fwd
    ON statement (subject_id, predicate_id, object_id)
    WHERE is_active;

-- 图遍历反向：object -> subject（callers / 被引用）
CREATE INDEX idx_stmt_rev
    ON statement (object_id, predicate_id, subject_id)
    WHERE is_active;

-- 按谓词检索
CREATE INDEX idx_stmt_pred
    ON statement (predicate_id)
    WHERE is_active;

-- 条目热字段表达式索引（$meta 等值过滤）
CREATE INDEX idx_knowledge_kind   ON knowledge ((meta->>'kind'))   WHERE is_active;
CREATE INDEX idx_knowledge_lang   ON knowledge ((meta->>'lang'))   WHERE is_active;
CREATE INDEX idx_knowledge_symbol ON knowledge ((meta->>'symbol')) WHERE is_active;
CREATE INDEX idx_knowledge_file   ON knowledge ((meta->>'file'))   WHERE is_active;

-- 全文检索（应用层预分词写入 search_tsv）
CREATE INDEX idx_knowledge_fti
    ON knowledge USING GIN (search_tsv)
    WHERE is_active;

-- JSONB 通用包含查询
CREATE INDEX idx_knowledge_meta
    ON knowledge USING GIN (meta jsonb_path_ops)
    WHERE is_active;

-- 语义向量（pgvector HNSW，余弦距离）
CREATE INDEX idx_knowledge_emb
    ON knowledge USING hnsw (embedding vector_cosine_ops);

COMMIT;
