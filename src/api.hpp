#pragma once
#include "compile.hpp"
#include "db.hpp"

#include <jansson.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

CompileOptions make_options(Db& db, const std::vector<VectorHit>& vectors,
                            std::unordered_map<std::string, long long>& cache);

// Hybrid search (vector + full-text RRF). Returns a new json_t* (caller decrefs).
// If qvec is non-empty, vector candidates come from pgvector ($knn); otherwise from
// the external provider (vector_cmd).
json_t* api_search(Db& db, const std::string& query, int k, const std::string& vector_cmd,
                   const std::string& scalar_json, const std::vector<float>& qvec = {});

// Context bundle for a key. Returns a new json_t* (caller decrefs).
json_t* api_context(Db& db, const std::string& key, int depth, int k, const std::string& pred_key,
                    const std::string& vector_cmd, bool no_content, long max_bytes);
