#include "write.hpp"

#include "util.hpp"

#include <jansson.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <unistd.h>

namespace kwrite {

namespace {

const char* P_ABOUT = "/pred/about";

std::string audit_key(const std::string& agent) {
    static std::atomic<unsigned> seq{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return "/audit/" + agent + "/" + std::to_string(ms) + "-" + std::to_string(getpid()) + "-" +
           std::to_string(seq++);
}

}  // namespace

const char* UPSERT_CONFLICT =
    "ON CONFLICT (key) DO UPDATE SET "
    "meta = EXCLUDED.meta, content = EXCLUDED.content, search_tsv = EXCLUDED.search_tsv, "
    "is_archived = false, end_time = NULL, updated_at = now(), "
    "version = CASE WHEN knowledge.meta IS DISTINCT FROM EXCLUDED.meta "
    "OR knowledge.content IS DISTINCT FROM EXCLUDED.content "
    "OR knowledge.search_tsv IS DISTINCT FROM EXCLUDED.search_tsv "
    "THEN knowledge.version + 1 ELSE knowledge.version END";

void upsert_entry(Db& db, const std::string& key, const std::string& meta_json,
                  const std::string& content_json, const std::string& terms) {
    std::string sql =
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) " +
        std::string(UPSERT_CONFLICT);
    db.exec(sql, {key, meta_json, content_json, terms});
}

void upsert_from_stage(Db& db, const char* stage_terms_expr) {
    std::string sql =
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "SELECT key, meta::jsonb, content::jsonb, " +
        std::string(stage_terms_expr) + " FROM stg_knowledge " + UPSERT_CONFLICT;
    db.exec(sql, {});
}

void ensure_predicate(Db& db, const std::string& key) {
    if (key.rfind("/pred/", 0) != 0) return;
    std::string name = key.substr(6);
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) VALUES ($1, $2::jsonb, '{}'::jsonb, "
        "to_tsvector('simple', $3)) ON CONFLICT (key) DO UPDATE SET is_archived = false, "
        "end_time = NULL",
        {key, std::string(R"({"category":"predicate","name":")") + name + R"("})", name});
}

long link_edge(Db& db, const std::string& s, const std::string& p, const std::string& o,
               bool strict) {
    ensure_predicate(db, p);
    db.exec(
        "INSERT INTO statement (subject_id, predicate_id, object_id) "
        "SELECT a.id, b.id, c.id FROM knowledge a, knowledge b, knowledge c "
        "WHERE a.key = $1 AND b.key = $2 AND c.key = $3 "
        "ON CONFLICT ON CONSTRAINT uq_stmt DO UPDATE SET is_archived = false, end_time = NULL",
        {s, p, o});

    json_t* rows = db.query_json(
        "SELECT count(*) AS c FROM statement st JOIN knowledge s ON s.id = st.subject_id "
        "JOIN knowledge p ON p.id = st.predicate_id JOIN knowledge o ON o.id = st.object_id "
        "WHERE s.key = $1 AND p.key = $2 AND o.key = $3",
        {s, p, o});
    long c = static_cast<long>(kutil::json_as_int(json_object_get(json_array_get(rows, 0), "c")));
    json_decref(rows);

    if (strict && c == 0) {
        json_t* chk = db.query_json(
            "SELECT (SELECT count(*) FROM knowledge WHERE key = $1) AS s, "
            "(SELECT count(*) FROM knowledge WHERE key = $2) AS p, "
            "(SELECT count(*) FROM knowledge WHERE key = $3) AS o",
            {s, p, o});
        json_t* row = json_array_get(chk, 0);
        std::string missing;
        if (kutil::json_as_int(json_object_get(row, "s")) == 0) missing += " subject=" + s;
        if (kutil::json_as_int(json_object_get(row, "p")) == 0) missing += " predicate=" + p;
        if (kutil::json_as_int(json_object_get(row, "o")) == 0) missing += " object=" + o;
        json_decref(chk);
        throw std::runtime_error("[LINK] edge endpoints do not exist:" + missing);
    }
    return c;
}

std::string audit(Db& db, const std::string& agent, const std::string& action,
                  const std::string& target, const std::string& detail_json) {
    std::string key = audit_key(agent.empty() ? "anon" : agent);
    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string("audit"));
    json_object_set_new(meta, "agent", json_string(agent.c_str()));
    json_object_set_new(meta, "action", json_string(action.c_str()));
    json_object_set_new(meta, "target", json_string(target.c_str()));
    json_object_set_new(meta, "ts", json_string(kutil::now_iso().c_str()));
    json_t* content = json_object();
    json_t* detail =
        detail_json.empty() ? nullptr : json_loads(detail_json.c_str(), JSON_DECODE_ANY, nullptr);
    json_object_set_new(content, "detail", detail ? detail : json_null());

    db.exec(std::string("INSERT INTO knowledge (key, meta, content, search_tsv) "
                        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) ") +
                UPSERT_CONFLICT,
            {key, kutil::dump_owned(meta), kutil::dump_owned(content),
             action + " " + target});

    if (!target.empty()) {
        // Lenient: audit rows must never fail the action that produced them.
        try {
            link_edge(db, key, P_ABOUT, target, false);
        } catch (...) {
        }
    }
    return key;
}

}  // namespace kwrite
