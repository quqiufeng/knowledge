#include "vector.hpp"

#include "shell.hpp"

#include <jansson.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

json_t* parse_json_tolerant(const std::string& out) {
    json_error_t err;
    json_t* root = json_loads(out.c_str(), 0, &err);
    if (root) return root;
    for (size_t pos = out.find_first_of("{["); pos != std::string::npos;
         pos = out.find_first_of("[{", pos + 1)) {
        root = json_loads(out.c_str() + pos, 0, &err);
        if (root) return root;
    }
    return nullptr;
}

std::string default_embed_cmd() {
    const char* env = std::getenv("KNOWLEDGE_EMBED_CMD");
    if (env && *env) return env;
    return "./tools/embed_query.sh";
}

}  // namespace

std::vector<float> parse_float_vector(const std::string& out) {
    json_t* root = parse_json_tolerant(out);
    if (!root) throw std::runtime_error("[EMBED] no JSON in embedder output");
    const json_t* arr = root;
    if (json_is_object(root)) {
        const json_t* r = json_object_get(root, "embedding");
        if (json_is_array(r)) arr = r;
    }
    std::vector<float> vec;
    if (json_is_array(arr)) {
        for (size_t i = 0; i < json_array_size(arr); ++i) {
            const json_t* e = json_array_get(arr, i);
            if (json_is_number(e)) vec.push_back(static_cast<float>(json_number_value(e)));
        }
    }
    json_decref(root);
    if (vec.empty()) throw std::runtime_error("[EMBED] embedder returned no numbers");
    return vec;
}

std::vector<float> embed_text(const std::string& text, const std::string& embed_cmd) {
    std::string cmd = embed_cmd.empty() ? default_embed_cmd() : embed_cmd;
    std::string out = ksh::run_checked(cmd + " " + ksh::quote(text));
    return parse_float_vector(out);
}

std::vector<VectorHit> run_vector_provider(const std::string& cmd, const std::string& query, int k) {
    std::vector<VectorHit> hits;
    if (cmd.empty()) return hits;

    std::string full = cmd + " " + ksh::quote(query) + " " + std::to_string(k);
    ksh::Result r = ksh::run(full);

    json_t* root = parse_json_tolerant(r.out);
    if (!root) {
        std::string why = r.timed_out   ? "timeout"
                          : r.size_capped ? "output limit exceeded"
                                          : "exit=" + std::to_string(r.rc);
        throw std::runtime_error("[VECTOR] provider produced no JSON (" + why +
                                 ", bytes=" + std::to_string(r.out.size()) + ")");
    }

    const json_t* arr = root;
    if (json_is_object(root)) {
        const json_t* res = json_object_get(root, "results");
        if (json_is_array(res)) arr = res;
    }
    if (!json_is_array(arr)) {
        json_decref(root);
        throw std::runtime_error("[VECTOR] provider JSON must be an array or {results:[]}");
    }

    for (size_t i = 0; i < json_array_size(arr); ++i) {
        const json_t* e = json_array_get(arr, i);
        const json_t* key = json_object_get(e, "key");
        if (!json_is_string(key)) continue;
        VectorHit hit;
        hit.key = json_string_value(key);
        const json_t* sc = json_object_get(e, "score");
        if (json_is_number(sc)) hit.score = json_number_value(sc);
        hits.push_back(hit);
    }
    json_decref(root);
    return hits;
}
