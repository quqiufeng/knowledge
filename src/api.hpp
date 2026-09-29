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

// key -> id resolver backed by a cache (used by compile for literal emission).
std::function<bool(const std::string&, long long&)> make_resolver(
    Db& db, std::unordered_map<std::string, long long>& cache);

// Hybrid search (vector + full-text RRF). Returns a new json_t* (caller decrefs).
// If qvec is non-empty, vector candidates come from pgvector ($knn); otherwise from
// the external provider (vector_cmd); with neither, the query is embedded via
// embed_cmd (default $KNOWLEDGE_EMBED_CMD) and pgvector is used.
json_t* api_search(Db& db, const std::string& query, int k, const std::string& vector_cmd,
                   const std::string& scalar_json, const std::vector<float>& qvec = {},
                   const std::string& embed_cmd = "");

// Context bundle for a key. Returns a new json_t* (caller decrefs).
// "related" is always present (empty array when no vector engine/embedder works).
json_t* api_context(Db& db, const std::string& key, int depth, int k, const std::string& pred_key,
                    const std::string& vector_cmd, bool no_content, long max_bytes,
                    bool exclude_headers = false);

// Apply --no-content / --max-code-bytes trimming to a "definition" array.
void api_trim_definition(json_t* defs, bool no_content, long max_bytes);

// One-hop call paths for key (recursive walk up to depth, depth<=0 -> empty array).
// Returns a new json_t* array (caller decrefs). Bounded by a row LIMIT.
constexpr int api_paths_limit = 500;
json_t* api_paths(Db& db, const std::string& key, int depth, const std::string& pred_key);
