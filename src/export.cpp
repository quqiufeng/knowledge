#include "export.hpp"

#include "api.hpp"

#include <jansson.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

std::string dump(const json_t* v) {
    char* s = json_dumps(v, JSON_COMPACT);
    std::string out = s ? s : "";
    if (s) free(s);
    return out;
}

long export_edges(Db& db, const ExportOptions& opt, FILE* out) {
    // One record per active edge: subject -predicate-> object (+ endpoint meta).
    json_t* rows = db.query_json(
        "SELECT s.key AS subject, p.key AS predicate, o.key AS object, "
        "       s.meta AS subject_meta, o.meta AS object_meta "
        "FROM statement st "
        "JOIN knowledge s ON s.id = st.subject_id "
        "JOIN knowledge p ON p.id = st.predicate_id "
        "JOIN knowledge o ON o.id = st.object_id "
        "WHERE st.is_active AND ($1 = '' OR p.key = $1) AND ($2 = '' OR s.key LIKE $2 || '%') "
        "LIMIT $3",
        {opt.predicate, opt.key_prefix, std::to_string(opt.limit)});

    long n = 0;
    for (size_t i = 0; i < json_array_size(rows); ++i) {
        const json_t* r = json_array_get(rows, i);
        json_t* rec = json_object();
        json_object_set(rec, "subject", json_object_get(r, "subject"));
        json_object_set(rec, "predicate", json_object_get(r, "predicate"));
        json_object_set(rec, "object", json_object_get(r, "object"));
        json_object_set(rec, "subject_meta", json_object_get(r, "subject_meta"));
        json_object_set(rec, "object_meta", json_object_get(r, "object_meta"));
        std::string line = dump(rec);
        json_decref(rec);
        std::fprintf(out, "%s\n", line.c_str());
        n++;
    }
    json_decref(rows);
    return n;
}

long export_context(Db& db, const ExportOptions& opt, FILE* out) {
    // One record per entry: full context bundle (definition + callers/callees + paths).
    json_t* keys = db.query_json(
        "SELECT key FROM knowledge WHERE is_active AND meta->>'kind' = 'function' "
        "AND ($1 = '' OR key LIKE $1 || '%') LIMIT $2",
        {opt.key_prefix, std::to_string(opt.limit)});

    long n = 0;
    for (size_t i = 0; i < json_array_size(keys); ++i) {
        const json_t* k = json_object_get(json_array_get(keys, i), "key");
        if (!json_is_string(k)) continue;
        json_t* bundle = api_context(db, json_string_value(k), opt.depth, 8, "/pred/calls", "", opt.no_content, 0);
        std::string line = dump(bundle);
        json_decref(bundle);
        std::fprintf(out, "%s\n", line.c_str());
        n++;
    }
    json_decref(keys);
    return n;
}

}  // namespace

long export_corpus(Db& db, const ExportOptions& opt) {
    if (opt.preset == "edges") return export_edges(db, opt, stdout);
    if (opt.preset == "context") return export_context(db, opt, stdout);
    throw std::runtime_error("[EXPORT] preset must be 'edges' or 'context'");
}
