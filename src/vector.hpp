#pragma once
#include "compile.hpp"

#include <string>
#include <vector>

std::vector<VectorHit> run_vector_provider(const std::string& cmd, const std::string& query, int k);

// Parse a JSON array of numbers (or {results:[...]}) into floats.
std::vector<float> parse_float_vector(const std::string& out);

// Embed one query via embed_cmd (default: $KNOWLEDGE_EMBED_CMD or
// ./tools/embed_query.sh). Throws on failure / timeout / size cap.
std::vector<float> embed_text(const std::string& text, const std::string& embed_cmd = "");
