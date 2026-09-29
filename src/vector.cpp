#include "vector.hpp"

#include <jansson.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string run_capture(const std::string& cmd, int& rc) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw std::runtime_error("[VECTOR] popen failed");
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    rc = pclose(p);
    return out;
}

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

}  // namespace

std::vector<VectorHit> run_vector_provider(const std::string& cmd, const std::string& query, int k) {
    std::vector<VectorHit> hits;
    if (cmd.empty()) return hits;

    std::string full = cmd + " " + shell_quote(query) + " " + std::to_string(k);
    int rc = 0;
    std::string out = run_capture(full, rc);

    json_t* root = parse_json_tolerant(out);
    if (!root) {
        throw std::runtime_error("[VECTOR] provider produced no JSON (exit=" + std::to_string(rc) +
                                 ", bytes=" + std::to_string(out.size()) + ")");
    }

    const json_t* arr = root;
    if (json_is_object(root)) {
        const json_t* r = json_object_get(root, "results");
        if (json_is_array(r)) arr = r;
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
