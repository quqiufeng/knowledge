#include "spec.hpp"

#include "util.hpp"
#include "write.hpp"

#include <jansson.h>

#include <stdexcept>
#include <string>

namespace {

const json_t* resolve(const json_t* entry, const std::string& path) {
    const json_t* cur = entry;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string seg = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!json_is_object(cur)) return nullptr;
        cur = json_object_get(cur, seg.c_str());
        if (!cur) return nullptr;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

bool type_ok(const json_t* v, const std::string& type) {
    if (type == "string") return json_is_string(v);
    if (type == "number") return json_is_number(v);
    if (type == "integer") return json_is_integer(v);
    if (type == "boolean") return json_is_boolean(v);
    if (type == "array") return json_is_array(v);
    if (type == "object") return json_is_object(v);
    return true;
}

}  // namespace

void spec_set(Db& db, const std::string& category, const std::string& spec_json) {
    if (category.empty()) throw std::runtime_error("[SPEC] category is required");
    json_error_t err;
    json_t* spec = json_loads(spec_json.c_str(), 0, &err);
    if (!json_is_object(spec)) {
        if (spec) json_decref(spec);
        throw std::runtime_error(std::string("[SPEC] spec must be a JSON object: ") + err.text);
    }
    std::string key = "/spec/" + category;
    std::string meta = std::string(R"({"category":"spec","applies_to":")") + category + R"("})";
    kwrite::upsert_entry(db, key, meta, kutil::dump_owned(spec), category);
}

json_t* spec_load(Db& db, const std::string& category) {
    if (category.empty()) return nullptr;
    std::string key = "/spec/" + category;
    json_t* rows = db.query_json("SELECT content FROM knowledge WHERE key = $1 AND is_active", {key});
    json_t* out = nullptr;
    if (json_array_size(rows) > 0) {
        const json_t* c = json_object_get(json_array_get(rows, 0), "content");
        if (json_is_object(c)) out = json_incref(const_cast<json_t*>(c));
    }
    json_decref(rows);
    return out;
}

void spec_validate(const json_t* spec, const json_t* entry, const std::string& key) {
    if (!spec) return;
    const json_t* required = json_object_get(spec, "required");
    if (json_is_array(required)) {
        for (size_t i = 0; i < json_array_size(required); ++i) {
            const json_t* p = json_array_get(required, i);
            if (!json_is_string(p)) continue;
            std::string path = json_string_value(p);
            if (!resolve(entry, path))
                throw std::runtime_error("[SPEC] " + key + " missing required field: " + path);
        }
    }
    const json_t* types = json_object_get(spec, "types");
    if (json_is_object(types)) {
        const char* path;
        json_t* t;
        json_object_foreach(const_cast<json_t*>(types), path, t) {
            if (!json_is_string(t)) continue;
            const json_t* v = resolve(entry, path);
            if (v && !type_ok(v, json_string_value(t)))
                throw std::runtime_error("[SPEC] " + key + " field " + path + " must be " +
                                         json_string_value(t));
        }
    }
}
