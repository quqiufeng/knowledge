#include "memory.hpp"

#include "util.hpp"
#include "write.hpp"

#include <jansson.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* P_ABOUT = "/pred/about";
const char* P_SUPERSEDES = "/pred/supersedes";

std::string event_key(const std::string& agent) {
    static std::atomic<unsigned> seq{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return "/mem/" + agent + "/events/" + std::to_string(ms) + "-" + std::to_string(getpid()) + "-" +
           std::to_string(seq++);
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
    json_object_set_new(meta, "ts", json_string(kutil::now_iso().c_str()));
    if (!tags.empty()) {
        json_t* arr = json_array();
        for (const auto& t : tags) json_array_append_new(arr, json_string(t.c_str()));
        json_object_set_new(meta, "tags", arr);
    }
    json_t* content = json_object();
    json_object_set_new(content, "text", json_string(text.c_str()));

    Db::Tx tx(db);
    kwrite::upsert_entry(db, key, kutil::dump_owned(meta), kutil::dump_owned(content), text);
    // Strict: a dangling --about key must fail loudly, not vanish silently.
    for (const auto& a : about) kwrite::link_edge(db, key, P_ABOUT, a, true);
    kwrite::audit(db, agent, "remember", key, "");
    tx.commit();
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

    Db::Tx tx(db);

    // Archive the previous value (history is data).
    json_t* prev = db.query_json("SELECT version, content FROM knowledge WHERE key = $1", {key});
    if (json_array_size(prev) > 0) {
        const json_t* row = json_array_get(prev, 0);
        std::string version = kutil::json_as_text(json_object_get(row, "version"));
        std::string old_content = kutil::dump_owned(json_incref(json_object_get(row, "content")));
        std::string archive_key = key + "/@archive/" + version;

        json_t* ameta = json_object();
        json_object_set_new(ameta, "kind", json_string("archive"));
        json_object_set_new(ameta, "of", json_string(key.c_str()));
        json_object_set_new(ameta, "version", json_string(version.c_str()));
        json_object_set_new(ameta, "agent", json_string(agent.c_str()));
        json_object_set_new(ameta, "ts", json_string(kutil::now_iso().c_str()));
        kwrite::upsert_entry(db, archive_key, kutil::dump_owned(ameta), old_content, old_content);
        kwrite::link_edge(db, key, P_SUPERSEDES, archive_key, false);
    }
    json_decref(prev);

    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string("fact"));
    json_object_set_new(meta, "agent", json_string(agent.c_str()));
    json_object_set_new(meta, "topic", json_string(topic.c_str()));
    if (!session.empty()) json_object_set_new(meta, "session", json_string(session.c_str()));
    json_object_set_new(meta, "ts", json_string(kutil::now_iso().c_str()));

    json_t* content = json_object();
    json_object_set_new(content, "value", value);

    std::string value_text = value_json;
    kwrite::upsert_entry(db, key, kutil::dump_owned(meta), kutil::dump_owned(content), value_text);
    kwrite::audit(db, agent, "fact", key, "");
    tx.commit();
    return key;
}

long memory_link(Db& db, const std::string& agent, const std::string& subject,
                 const std::string& predicate, const std::string& object) {
    if (subject.empty() || predicate.empty() || object.empty())
        throw std::runtime_error("[MEMORY] link needs subject, predicate, object");
    Db::Tx tx(db);
    long c = kwrite::link_edge(db, subject, predicate, object, true);
    kwrite::audit(db, agent, "link", subject,
          std::string(R"({"predicate":")") + predicate + R"(","object":")" + object + R"("})");
    tx.commit();
    return c;
}
