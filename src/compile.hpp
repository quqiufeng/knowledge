#pragma once
#include <jansson.h>

#include <functional>
#include <string>
#include <vector>

struct VectorHit {
    std::string key;
    double score{0.0};
};

struct CompiledQuery {
    std::string sql;
    std::vector<std::string> params;
};

struct CompileOptions {
    std::string alias{"knowledge"};
    std::vector<VectorHit> vectors;
    // Optional: resolve a knowledge key to its id at compile time (emits an integer
    // literal instead of a per-condition subquery). Return false to fall back.
    std::function<bool(const std::string& key, long long& id)> resolve_key;
};

CompiledQuery compile_query(const json_t* query, const CompileOptions& opts = {});
