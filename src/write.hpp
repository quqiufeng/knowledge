#pragma once
#include "db.hpp"

#include <string>

namespace kwrite {

// Shared INSERT conflict action: refresh content, reactivate (explicit write/import
// wins over archive), bump version only when meta/content/search_tsv actually changed.
extern const char* UPSERT_CONFLICT;

// Upsert one knowledge row. version increments only when the content actually
// changes (meta/content/search_tsv), so idempotent writes don't churn version.
void upsert_entry(Db& db, const std::string& key, const std::string& meta_json,
                  const std::string& content_json, const std::string& terms);

// Bulk upsert from a stage table that mirrors knowledge's column names
// (key, meta, content, terms) using the same conflict action as upsert_entry.
void upsert_from_stage(Db& db, const char* stage_terms_expr = "to_tsvector('simple', terms)");

// Ensure a /pred/* predicate entry exists. No-op for keys outside /pred/.
void ensure_predicate(Db& db, const std::string& key);

// Insert an edge (idempotent). With strict=true, verify all three endpoints
// exist and throw a clear error otherwise; returns the edge count either way.
long link_edge(Db& db, const std::string& subject, const std::string& predicate,
               const std::string& object, bool strict);

// Append an /audit/... row plus its /pred/about edge to the target (if any).
std::string audit(Db& db, const std::string& agent, const std::string& action,
                  const std::string& target, const std::string& detail_json);

}  // namespace kwrite
