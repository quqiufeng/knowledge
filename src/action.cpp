#include "action.hpp"

#include "spec.hpp"
#include "util.hpp"

#include <jansson.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

const char* P_ABOUT = "/pred/about";

void ensure_predicate(Db& db, const std::string& key, const char* name) {
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) VALUES ($1, $2::jsonb, '{}'::jsonb, "
        "to_tsvector('simple', $3)) ON CONFLICT (key) DO NOTHING",
        {key, std::string(R"({"category":"predicate","name":")") + name + R"("})", name});
}

std::string now_iso() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

std::string audit_key(const std::string& agent) {
    static std::atomic<unsigned> seq{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return "/audit/" + agent + "/" + std::to_string(ms) + "-" + std::to_string(getpid()) + "-" +
           std::to_string(seq++);
}

}  // namespace

std::string audit(Db& db, const std::string& agent, const std::string& action,
                  const std::string& target, const std::string& detail_json) {
    std::string key = audit_key(agent.empty() ? "anon" : agent);
    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string("audit"));
    json_object_set_new(meta, "agent", json_string(agent.c_str()));
    json_object_set_new(meta, "action", json_string(action.c_str()));
    json_object_set_new(meta, "target", json_string(target.c_str()));
    json_object_set_new(meta, "ts", json_string(now_iso().c_str()));
    json_t* content = json_object();
    json_t* detail = detail_json.empty() ? json_null() : json_loads(detail_json.c_str(), JSON_DECODE_ANY, nullptr);
    json_object_set_new(content, "detail", detail ? detail : json_null());

    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()",
        {key, kutil::dump_owned(meta), kutil::dump_owned(content), action + " " + target});

    if (!target.empty()) {
        ensure_predicate(db, P_ABOUT, "about");
        db.exec(
            "INSERT INTO statement (subject_id, predicate_id, object_id) "
            "SELECT a.id, p.id, o.id FROM knowledge a, knowledge p, knowledge o "
            "WHERE a.key = $1 AND p.key = $2 AND o.key = $3 ON CONFLICT DO NOTHING",
            {key, P_ABOUT, target});
    }
    return key;
}

std::string action_put(Db& db, const std::string& agent, const std::string& key,
                       const std::string& meta_json, const std::string& content_json,
                       const std::string& category, const std::string& text) {
    if (key.empty()) throw std::runtime_error("[ACTION] put requires a key");

    json_error_t err;
    json_t* meta = json_loads(meta_json.empty() ? "{}" : meta_json.c_str(), 0, &err);
    if (!json_is_object(meta)) {
        if (meta) json_decref(meta);
        throw std::runtime_error(std::string("[ACTION] invalid meta JSON: ") + err.text);
    }
    json_t* content = json_loads(content_json.empty() ? "{}" : content_json.c_str(), 0, &err);
    if (!json_is_object(content)) {
        if (content) json_decref(content);
        json_decref(meta);
        throw std::runtime_error(std::string("[ACTION] invalid content JSON: ") + err.text);
    }

    if (!category.empty()) {
        json_t* spec = spec_load(db, category);
        if (spec) {
            json_t* entry = json_object();
            json_object_set_new(entry, "key", json_string(key.c_str()));
            json_object_set(entry, "meta", meta);
            json_object_set(entry, "content", content);
            try {
                spec_validate(spec, entry, key);
            } catch (...) {
                json_decref(entry);
                json_decref(spec);
                json_decref(meta);
                json_decref(content);
                throw;
            }
            json_decref(entry);
            json_decref(spec);
        }
    }

    std::string terms = text;
    if (terms.empty()) terms = kutil::dump_owned(json_deep_copy(content));
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()",
        {key, kutil::dump_owned(meta), kutil::dump_owned(content), terms});

    audit(db, agent, "put", key, "");
    return key;
}

bool action_forget(Db& db, const std::string& agent, const std::string& key, bool force) {
    if (key.empty()) throw std::runtime_error("[ACTION] forget requires a key");
    if (!force && key.rfind("/mem/", 0) != 0)
        throw std::runtime_error("[ACTION] forget only applies to /mem/ keys (use --force to override)");
    json_t* rows = db.query_json(
        "UPDATE knowledge SET is_archived = true, end_time = now(), version = version + 1, "
        "updated_at = now() WHERE key = $1 AND is_active RETURNING key",
        {key});
    bool changed = json_array_size(rows) > 0;
    json_decref(rows);
    if (changed) audit(db, agent, "forget", key, "");
    return changed;
}

bool action_restore(Db& db, const std::string& agent, const std::string& key) {
    if (key.empty()) throw std::runtime_error("[ACTION] restore requires a key");
    json_t* rows = db.query_json(
        "UPDATE knowledge SET is_archived = false, end_time = NULL, version = version + 1, "
        "updated_at = now() WHERE key = $1 AND NOT is_active RETURNING key",
        {key});
    bool changed = json_array_size(rows) > 0;
    json_decref(rows);
    if (changed) audit(db, agent, "restore", key, "");
    return changed;
}
