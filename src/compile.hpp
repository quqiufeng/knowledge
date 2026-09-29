#pragma once
#include <jansson.h>

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

CompiledQuery compile_query(const json_t* query, const std::vector<VectorHit>& vectors);
