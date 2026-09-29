#include "export.hpp"

#include "api.hpp"
#include "util.hpp"

#include <jansson.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string dump(const json_t* v) {
    char* s = json_dumps(v, JSON_COMPACT);
    std::string out = s ? s : "";
    if (s) free(s);
    return out;
}

long export_edges(Db& db, const ExportOptions& opt, FILE* out) {
    // One record per active edge: subject -predicate-> object (+ endpoint meta).
    // Ordered by edge id so reruns produce byte-identical corpora.
    json_t* rows = db.query_json(
        "SELECT s.key AS subject, p.key AS predicate, o.key AS object, "
        "       s.meta AS subject_meta, o.meta AS object_meta "
        "FROM statement st "
        "JOIN knowledge s ON s.id = st.subject_id "
        "JOIN knowledge p ON p.id = st.predicate_id "
        "JOIN knowledge o ON o.id = st.object_id "
        "WHERE st.is_active AND s.is_active AND o.is_active "
        "  AND ($1 = '' OR p.key = $1) AND ($2 = '' OR starts_with(s.key, $2)) "
        "ORDER BY st.id LIMIT $3",
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

// Group one batched edge result by anchor id.
using EdgeMap = std::unordered_map<long long, std::vector<json_t*> >;

EdgeMap group_edges(json_t* rows, bool callee_side) {
    EdgeMap m;
    for (size_t i = 0; i < json_array_size(rows); ++i) {
        json_t* r = json_array_get(rows, i);
        long long anchor = kutil::json_as_int(json_object_get(r, "anchor_id"), -1);
        bool as_callee = json_is_true(json_object_get(r, "as_callee"));
        if (as_callee != callee_side) continue;
        m[anchor].push_back(r);
    }
    return m;
}

json_t* rel_array(const std::vector<json_t*>& rows) {
    json_t* arr = json_array();
    for (json_t* r : rows) {
        json_t* o = json_object();
        json_object_set(o, "key", json_object_get(r, "key"));
        json_object_set(o, "meta", json_object_get(r, "meta"));
        json_array_append_new(arr, o);
    }
    return arr;
}

long export_context(Db& db, const ExportOptions& opt, FILE* out) {
    // One record per entry: definition + callers/callees + paths. All relation
    // lookups are batched (no per-key queries) and every query has a stable order.
    json_t* keys = db.query_json(
        "SELECT id, key FROM knowledge WHERE is_active AND meta->>'kind' = 'function' "
        "AND ($1 = '' OR starts_with(key, $1)) ORDER BY id LIMIT $2",
        {opt.key_prefix, std::to_string(opt.limit)});
    if (json_array_size(keys) == 0) {
        json_decref(keys);
        return 0;
    }

    std::vector<long long> ids;
    std::vector<std::string> kstrs;
    std::string id_list = "{";
    for (size_t i = 0; i < json_array_size(keys); ++i) {
        const json_t* r = json_array_get(keys, i);
        long long id = kutil::json_as_int(json_object_get(r, "id"));
        ids.push_back(id);
        kstrs.push_back(kutil::json_as_text(json_object_get(r, "key")));
        if (i) id_list += ",";
        id_list += std::to_string(id);
    }
    id_list += "}";

    json_t* defs = db.query_json(
        "SELECT id, key, meta, content, version, updated_at FROM knowledge "
        "WHERE id = ANY($1::bigint[]) AND is_active",
        {id_list});
    api_trim_definition(defs, opt.no_content, 0);
    std::unordered_map<long long, json_t*> def_by_id;
    for (size_t i = 0; i < json_array_size(defs); ++i) {
        json_t* r = json_array_get(defs, i);
        def_by_id[kutil::json_as_int(json_object_get(r, "id"))] = r;
    }

    json_t* edges = db.query_json(
        "SELECT st.object_id AS anchor_id, k.key AS key, k.meta AS meta, FALSE AS as_callee "
        "FROM statement st JOIN knowledge k ON k.id = st.subject_id "
        "WHERE st.object_id = ANY($1::bigint[]) AND st.is_active AND k.is_active "
        "UNION ALL "
        "SELECT st.subject_id, k.key, k.meta, TRUE "
        "FROM statement st JOIN knowledge k ON k.id = st.object_id "
        "WHERE st.subject_id = ANY($1::bigint[]) AND st.is_active AND k.is_active "
        "ORDER BY 1, 2",
        {id_list});
    EdgeMap callers = group_edges(edges, false);
    EdgeMap callees = group_edges(edges, true);

    long n = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        auto fit = def_by_id.find(ids[i]);
        if (fit == def_by_id.end()) continue;  // archived between queries

        json_t* bundle = json_object();
        json_object_set_new(bundle, "key", json_string(kstrs[i].c_str()));

        json_t* defarr = json_array();
        json_array_append(defarr, fit->second);  // increfs
        json_object_set_new(bundle, "definition", defarr);

        auto cit = callers.find(ids[i]);
        json_object_set_new(bundle, "callers",
                            rel_array(cit == callers.end() ? std::vector<json_t*>{} : cit->second));
        auto cal = callees.find(ids[i]);
        json_object_set_new(bundle, "callees",
                            rel_array(cal == callees.end() ? std::vector<json_t*>{} : cal->second));

        if (opt.depth > 0) {
            json_t* paths = api_paths(db, kstrs[i], opt.depth, "/pred/calls");
            if (json_array_size(paths) >= (size_t)api_paths_limit)
                json_object_set_new(bundle, "paths_truncated", json_true());
            json_object_set_new(bundle, "paths", paths);
        }

        std::string line = dump(bundle);
        json_decref(bundle);
        std::fprintf(out, "%s\n", line.c_str());
        n++;
    }

    json_decref(edges);
    json_decref(defs);
    json_decref(keys);
    return n;
}

}  // namespace

long export_corpus(Db& db, const ExportOptions& opt) {
    if (opt.preset == "edges") return export_edges(db, opt, stdout);
    if (opt.preset == "context") return export_context(db, opt, stdout);
    throw std::runtime_error("[EXPORT] preset must be 'edges' or 'context'");
}
