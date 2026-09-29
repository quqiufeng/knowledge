#include "action.hpp"

#include "spec.hpp"
#include "util.hpp"
#include "write.hpp"

#include <jansson.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <unistd.h>

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

    Db::Tx tx(db);
    kwrite::upsert_entry(db, key, kutil::dump_owned(meta), kutil::dump_owned(content), terms);
    kwrite::audit(db, agent, "put", key, "");
    tx.commit();
    return key;
}

bool action_forget(Db& db, const std::string& agent, const std::string& key, bool force) {
    if (key.empty()) throw std::runtime_error("[ACTION] forget requires a key");
    if (!force && key.rfind("/mem/", 0) != 0)
        throw std::runtime_error("[ACTION] forget only applies to /mem/ keys (use --force to override)");
    Db::Tx tx(db);
    json_t* rows = db.query_json(
        "UPDATE knowledge SET is_archived = true, end_time = now(), version = version + 1, "
        "updated_at = now() WHERE key = $1 AND is_active RETURNING key",
        {key});
    bool changed = json_array_size(rows) > 0;
    json_decref(rows);
    if (changed) kwrite::audit(db, agent, "forget", key, "");
    tx.commit();
    return changed;
}

bool action_restore(Db& db, const std::string& agent, const std::string& key) {
    if (key.empty()) throw std::runtime_error("[ACTION] restore requires a key");
    Db::Tx tx(db);
    json_t* rows = db.query_json(
        "UPDATE knowledge SET is_archived = false, end_time = NULL, version = version + 1, "
        "updated_at = now() WHERE key = $1 AND NOT is_active RETURNING key",
        {key});
    bool changed = json_array_size(rows) > 0;
    json_decref(rows);
    if (changed) kwrite::audit(db, agent, "restore", key, "");
    tx.commit();
    return changed;
}
