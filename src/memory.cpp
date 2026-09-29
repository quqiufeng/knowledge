#include "memory.hpp"

#include "action.hpp"
#include "util.hpp"

#include <jansson.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* P_ABOUT = "/pred/about";
const char* P_SUPERSEDES = "/pred/supersedes";

void ensure_predicate(Db& db, const std::string& key) {
    if (key.rfind("/pred/", 0) != 0) return;
    std::string name = key.substr(6);
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

std::string event_key(const std::string& agent) {
    static std::atomic<unsigned> seq{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return "/mem/" + agent + "/events/" + std::to_string(ms) + "-" + std::to_string(getpid()) + "-" +
           std::to_string(seq++);
}

std::string upsert_memory(Db& db, const std::string& key, const std::string& meta_json,
                          const std::string& content_json, const std::string& text) {
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()",
        {key, meta_json, content_json, text});
    return key;
}

void link(Db& db, const std::string& s, const std::string& p, const std::string& o) {
    ensure_predicate(db, p);
    db.exec(
        "INSERT INTO statement (subject_id, predicate_id, object_id) "
        "SELECT s.id, p.id, o.id FROM knowledge s, knowledge p, knowledge o "
        "WHERE s.key = $1 AND p.key = $2 AND o.key = $3 ON CONFLICT DO NOTHING",
        {s, p, o});
}

}  // namespace

std::string memory_remember(Db& db, const std::string& agent, const std::string& session,
                            const std::string& text, const std::vector<std::string>& about,
                            const std::vector<std::string>& tags) {
    if (agent.empty()) throw std::runtime_error("[MEMORY] --agent is required");
    if (text.empty()) throw std::runtime_error("[MEMORY] --text is required");

    std::string key = event_key(agent);
    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string("event"));
    json_object_set_new(meta, "agent", json_string(agent.c_str()));
    if (!session.empty()) json_object_set_new(meta, "session", json_string(session.c_str()));
    json_object_set_new(meta, "ts", json_string(now_iso().c_str()));
    if (!tags.empty()) {
        json_t* arr = json_array();
        for (const auto& t : tags) json_array_append_new(arr, json_string(t.c_str()));
        json_object_set_new(meta, "tags", arr);
    }
    json_t* content = json_object();
    json_object_set_new(content, "text", json_string(text.c_str()));

    upsert_memory(db, key, kutil::dump_owned(meta), kutil::dump_owned(content), text);
    for (const auto& a : about) link(db, key, P_ABOUT, a);
    audit(db, agent, "remember", key, "");
    return key;
}

std::string memory_fact(Db& db, const std::string& agent, const std::string& topic,
                        const std::string& value_json, const std::string& session) {
    if (agent.empty()) throw std::runtime_error("[MEMORY] --agent is required");
    if (topic.empty()) throw std::runtime_error("[MEMORY] --topic is required");

    std::string key = "/mem/" + agent + "/facts/" + topic;

    json_error_t err;
    json_t* value = json_loads(value_json.c_str(), JSON_DECODE_ANY, &err);
    if (!value) throw std::runtime_error(std::string("[MEMORY] invalid --value JSON: ") + err.text);

    // Archive the previous value (history is data).
    json_t* prev = db.query_json("SELECT version, content FROM knowledge WHERE key = $1", {key});
    if (json_array_size(prev) > 0) {
        const json_t* row = json_array_get(prev, 0);
        std::string version = json_string_value(json_object_get(row, "version"));
        std::string old_content = kutil::dump_owned(json_incref(json_object_get(row, "content")));
        std::string archive_key = key + "/@archive/" + version;

        json_t* ameta = json_object();
        json_object_set_new(ameta, "kind", json_string("archive"));
        json_object_set_new(ameta, "of", json_string(key.c_str()));
        json_object_set_new(ameta, "version", json_string(version.c_str()));
        json_object_set_new(ameta, "agent", json_string(agent.c_str()));
        json_object_set_new(ameta, "ts", json_string(now_iso().c_str()));
        upsert_memory(db, archive_key, kutil::dump_owned(ameta), old_content, old_content);
        link(db, key, P_SUPERSEDES, archive_key);
    }
    json_decref(prev);

    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string("fact"));
    json_object_set_new(meta, "agent", json_string(agent.c_str()));
    json_object_set_new(meta, "topic", json_string(topic.c_str()));
    if (!session.empty()) json_object_set_new(meta, "session", json_string(session.c_str()));
    json_object_set_new(meta, "ts", json_string(now_iso().c_str()));

    json_t* content = json_object();
    json_object_set_new(content, "value", value);

    std::string value_text = value_json;
    upsert_memory(db, key, kutil::dump_owned(meta), kutil::dump_owned(content), value_text);
    audit(db, agent, "fact", key, "");
    return key;
}

long memory_link(Db& db, const std::string& agent, const std::string& subject,
                 const std::string& predicate, const std::string& object) {
    if (subject.empty() || predicate.empty() || object.empty())
        throw std::runtime_error("[MEMORY] link needs subject, predicate, object");
    link(db, subject, predicate, object);
    audit(db, agent, "link", subject, std::string(R"({"predicate":")") + predicate + R"(","object":")" + object + R"("})");
    json_t* rows = db.query_json(
        "SELECT count(*) AS c FROM statement st JOIN knowledge s ON s.id = st.subject_id "
        "JOIN knowledge p ON p.id = st.predicate_id JOIN knowledge o ON o.id = st.object_id "
        "WHERE s.key = $1 AND p.key = $2 AND o.key = $3",
        {subject, predicate, object});
    long c = std::atol(json_string_value(json_object_get(json_array_get(rows, 0), "c")));
    json_decref(rows);
    return c;
}

bool memory_forget(Db& db, const std::string& key, bool force) {
    if (key.empty()) throw std::runtime_error("[MEMORY] forget needs a key");
    if (!force && key.rfind("/mem/", 0) != 0)
        throw std::runtime_error("[MEMORY] forget only applies to /mem/ keys (use --force to override)");
    json_t* rows = db.query_json(
        "UPDATE knowledge SET is_archived = true, end_time = now(), version = version + 1, "
        "updated_at = now() WHERE key = $1 AND is_active RETURNING key",
        {key});
    bool changed = json_array_size(rows) > 0;
    json_decref(rows);
    return changed;
}
