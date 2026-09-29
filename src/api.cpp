#include "api.hpp"

#include "vector.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

std::function<bool(const std::string&, long long&)> make_resolver(
    Db& db, std::unordered_map<std::string, long long>& cache) {
    return [&db, &cache](const std::string& key, long long& id) -> bool {
        auto it = cache.find(key);
        if (it != cache.end()) {
            id = it->second;
            return true;
        }
        json_t* rows = db.query_json("SELECT id FROM knowledge WHERE key = $1", {key});
        bool found = json_array_size(rows) > 0;
        if (found) {
            const json_t* v = json_object_get(json_array_get(rows, 0), "id");
            id = std::atoll(json_string_value(v));
            cache[key] = id;
        }
        json_decref(rows);
        return found;
    };
}

CompileOptions make_options(Db& db, const std::vector<VectorHit>& vectors,
                            std::unordered_map<std::string, long long>& cache) {
    CompileOptions opts;
    opts.vectors = vectors;
    opts.resolve_key = make_resolver(db, cache);
    return opts;
}

namespace {

std::string pg_array_literal(const std::vector<std::string>& items) {
    std::string out = "{";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += '"';
        for (char c : items[i]) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
    }
    out += "}";
    return out;
}

void trim_definition(json_t* defs, bool no_content, long max_bytes) {
    const char* text_fields[] = {"code", "text"};
    for (size_t i = 0; i < json_array_size(defs); ++i) {
        json_t* content = json_object_get(json_array_get(defs, i), "content");
        if (!json_is_object(content)) continue;
        for (const char* field : text_fields) {
            if (no_content) {
                json_object_del(content, field);
                continue;
            }
            if (max_bytes > 0) {
                json_t* v = json_object_get(content, field);
                if (json_is_string(v) && (long)json_string_length(v) > max_bytes) {
                    std::string s = json_string_value(v);
                    s = s.substr(0, (size_t)max_bytes) + "\n... [truncated]";
                    json_object_set_new(content, field, json_string(s.c_str()));
                }
            }
        }
    }
}

}  // namespace

json_t* api_search(Db& db, const std::string& query, int k, const std::string& vector_cmd,
                   const std::string& scalar_json, const std::vector<float>& qvec) {
    json_error_t err;
    std::unordered_map<std::string, long long> cache;
    std::vector<VectorHit> hits;

    if (!qvec.empty()) {
        // pgvector path: vector candidates from the DB itself.
        json_t* varr = json_array();
        for (float f : qvec) json_array_append_new(varr, json_real(f));
        json_t* knn = json_object();
        json_object_set_new(knn, "vector", varr);
        json_object_set_new(knn, "k", json_integer(k));
        json_t* knn_node = json_object();
        json_object_set_new(knn_node, "$knn", knn);
        json_t* and_arr = json_array();
        json_array_append_new(and_arr, knn_node);
        if (!scalar_json.empty()) {
            json_t* extra = json_loads(scalar_json.c_str(), 0, &err);
            if (!extra) throw std::runtime_error(std::string("[INPUT] invalid scalar JSE: ") + err.text);
            json_array_append_new(and_arr, extra);
        }
        json_t* inner = json_object();
        json_object_set_new(inner, "$and", and_arr);
        json_t* q = json_object();
        json_object_set_new(q, "$where", inner);
        json_t* proj = json_array();
        json_array_append_new(proj, json_string("key"));
        json_object_set_new(q, "$project", proj);
        json_object_set_new(q, "$limit", json_integer(k));
        CompiledQuery cq = compile_query(q, make_options(db, {}, cache));
        json_t* vrows = db.query_json(cq.sql, cq.params);
        for (size_t i = 0; i < json_array_size(vrows); ++i) {
            const json_t* rk = json_object_get(json_array_get(vrows, i), "key");
            if (json_is_string(rk)) hits.push_back(VectorHit{json_string_value(rk), 0.0});
        }
        json_decref(vrows);
        json_decref(q);
    } else {
        hits = run_vector_provider(vector_cmd, query, k);
    }

    std::vector<std::string> lex_keys;
    {
        json_t* lq = json_object();
        json_t* lwhere = json_object();
        json_object_set_new(lwhere, "$fti", json_string(query.c_str()));
        json_object_set_new(lq, "$where", lwhere);
        json_t* lproj = json_array();
        json_array_append_new(lproj, json_string("key"));
        json_object_set_new(lq, "$project", lproj);
        json_t* lorder = json_object();
        json_object_set_new(lorder, "$fti_rank", json_string("desc"));
        json_object_set_new(lq, "$order", lorder);
        json_object_set_new(lq, "$limit", json_integer(k));
        CompiledQuery lc = compile_query(lq, make_options(db, {}, cache));
        json_t* lrows = db.query_json(lc.sql, lc.params);
        for (size_t i = 0; i < json_array_size(lrows); ++i) {
            const json_t* rk = json_object_get(json_array_get(lrows, i), "key");
            if (json_is_string(rk)) lex_keys.push_back(json_string_value(rk));
        }
        json_decref(lrows);
        json_decref(lq);
    }

    // Weighted RRF: semantic is the primary signal for code search; lexical is a
    // recall booster. Equal weights let lexical noise tie the correct vector hit.
    const double RRF_K = 60.0;
    const double W_VEC = 1.0;
    const double W_LEX = 0.5;
    std::unordered_map<std::string, double> rrf;
    std::unordered_map<std::string, int> vrank, lrank;
    std::unordered_map<std::string, double> vscore;
    for (size_t i = 0; i < hits.size(); ++i) {
        rrf[hits[i].key] += W_VEC / (RRF_K + static_cast<double>(i + 1));
        vrank[hits[i].key] = static_cast<int>(i + 1);
        vscore[hits[i].key] = hits[i].score;
    }
    for (size_t i = 0; i < lex_keys.size(); ++i) {
        const std::string& key = lex_keys[i];
        rrf[key] += W_LEX / (RRF_K + static_cast<double>(i + 1));
        if (!lrank.count(key)) lrank[key] = static_cast<int>(i + 1);
    }

    std::vector<VectorHit> fused;
    for (const auto& kv : rrf) fused.push_back(VectorHit{kv.first, kv.second});
    std::sort(fused.begin(), fused.end(), [](const VectorHit& a, const VectorHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.key < b.key;
    });
    if (static_cast<int>(fused.size()) > k) fused.resize(k);

    json_t* out = json_object();
    json_object_set_new(out, "query", json_string(query.c_str()));
    if (fused.empty()) {
        json_object_set_new(out, "source", json_string("none"));
        json_object_set_new(out, "results", json_array());
        return out;
    }

    json_t* body = json_object();
    json_t* and_arr = json_array();
    json_t* search_inner = json_object();
    json_object_set_new(search_inner, "query", json_string(query.c_str()));
    json_object_set_new(search_inner, "k", json_integer(k));
    json_t* search_node = json_object();
    json_object_set_new(search_node, "$search", search_inner);
    json_array_append_new(and_arr, search_node);
    if (!scalar_json.empty()) {
        json_t* extra = json_loads(scalar_json.c_str(), 0, &err);
        if (!extra) {
            json_decref(out);
            throw std::runtime_error(std::string("[INPUT] invalid scalar JSE: ") + err.text);
        }
        json_array_append_new(and_arr, extra);
    }
    json_t* where = json_object();
    json_object_set_new(where, "$and", and_arr);
    json_object_set_new(body, "$where", where);

    json_t* project = json_array();
    json_array_append_new(project, json_string("key"));
    json_array_append_new(project, json_string("meta.symbol"));
    json_array_append_new(project, json_string("meta.file"));
    json_array_append_new(project, json_string("meta.line"));
    json_array_append_new(project, json_string("meta.signature"));
    json_object_set_new(body, "$project", project);
    json_t* order = json_object();
    json_object_set_new(order, "$search_score", json_string("desc"));
    json_object_set_new(body, "$order", order);
    json_object_set_new(body, "$limit", json_integer(k));

    CompiledQuery cq = compile_query(body, make_options(db, fused, cache));
    json_t* rows = db.query_json(cq.sql, cq.params);
    json_decref(body);

    for (size_t i = 0; i < json_array_size(rows); ++i) {
        json_t* row = json_array_get(rows, i);
        const json_t* rk = json_object_get(row, "key");
        if (!json_is_string(rk)) continue;
        std::string key = json_string_value(rk);
        auto it = rrf.find(key);
        if (it != rrf.end()) json_object_set_new(row, "rrf", json_real(it->second));
        auto vr = vrank.find(key);
        if (vr != vrank.end()) json_object_set_new(row, "vector_rank", json_integer(vr->second));
        auto lr = lrank.find(key);
        if (lr != lrank.end()) json_object_set_new(row, "lexical_rank", json_integer(lr->second));
        auto vs = vscore.find(key);
        if (vs != vscore.end()) json_object_set_new(row, "vector_score", json_real(vs->second));
    }

    const char* source = !qvec.empty() ? "pgvector+fti" : (vector_cmd.empty() ? "pg+fti" : "vector+fti");
    json_object_set_new(out, "source", json_string(source));
    json_object_set_new(out, "results", rows);
    return out;
}

json_t* api_context(Db& db, const std::string& key, int depth, int k, const std::string& pred_key,
                    const std::string& vector_cmd, bool no_content, long max_bytes) {
    json_t* bundle = json_object();
    json_object_set_new(bundle, "key", json_string(key.c_str()));

    json_t* defs = db.query_json(
        "SELECT key, meta, content, version, updated_at FROM knowledge WHERE key = $1 AND is_active", {key});
    if (json_array_size(defs) == 0) {
        json_decref(defs);
        json_decref(bundle);
        throw std::runtime_error("[CONTEXT] key not found or inactive: " + key);
    }
    trim_definition(defs, no_content, max_bytes);
    json_object_set_new(bundle, "definition", defs);

    json_t* idrows = db.query_json("SELECT id FROM knowledge WHERE key = $1", {key});
    std::string id = json_string_value(json_object_get(json_array_get(idrows, 0), "id"));
    json_decref(idrows);

    json_object_set_new(
        bundle, "callers",
        db.query_json("SELECT k.key, k.meta FROM statement st JOIN knowledge k ON k.id = st.subject_id "
                      "WHERE st.object_id = $1::bigint AND st.is_active ORDER BY k.key",
                      {id}));
    json_object_set_new(
        bundle, "callees",
        db.query_json("SELECT k.key, k.meta FROM statement st JOIN knowledge k ON k.id = st.object_id "
                      "WHERE st.subject_id = $1::bigint AND st.is_active ORDER BY k.key",
                      {id}));

    if (depth > 0) {
        std::string preds = pg_array_literal({pred_key});
        json_object_set_new(
            bundle, "paths",
            db.query_json(
                "WITH RECURSIVE chain AS ("
                " SELECT k.id AS cur, 0 AS depth, ARRAY[k.id] AS ids, ARRAY[k.key]::text[] AS keys"
                " FROM knowledge k WHERE k.key = $1 AND k.is_active"
                " UNION ALL"
                " SELECT st.object_id, c.depth + 1, c.ids || st.object_id, c.keys || ok.key"
                " FROM chain c JOIN statement st ON st.subject_id = c.cur"
                " JOIN knowledge ok ON ok.id = st.object_id"
                " WHERE c.depth < $2::int AND st.is_active"
                "   AND st.predicate_id IN (SELECT id FROM knowledge WHERE key = ANY($3::text[]))"
                "   AND NOT (st.object_id = ANY(c.ids))"
                ") SELECT depth, keys FROM chain WHERE depth > 0 ORDER BY depth, keys",
                {key, std::to_string(depth), preds}));
    }

    if (!vector_cmd.empty()) {
        json_t* symbol_v = json_object_get(json_array_get(defs, 0), "meta");
        std::string symbol;
        if (json_is_object(symbol_v)) {
            const json_t* s = json_object_get(symbol_v, "symbol");
            if (json_is_string(s)) symbol = json_string_value(s);
        }
        std::vector<VectorHit> hits = run_vector_provider(vector_cmd, symbol.empty() ? key : symbol, k);
        std::vector<VectorHit> filtered;
        for (const auto& h : hits)
            if (h.key != key) filtered.push_back(h);

        if (!filtered.empty()) {
            std::string values;
            std::vector<std::string> params;
            for (size_t i = 0; i < filtered.size(); ++i) {
                if (i) values += ", ";
                std::string kp = "$" + std::to_string(params.size() + 1);
                params.push_back(filtered[i].key);
                std::string sp = "$" + std::to_string(params.size() + 1);
                params.push_back(std::to_string(filtered[i].score));
                values += "(" + kp + "::text, " + sp + "::double precision)";
            }
            json_object_set_new(
                bundle, "related",
                db.query_json(
                    "WITH vs(key, score) AS (VALUES " + values + ") "
                    "SELECT k.key, k.meta, vs.score FROM vs JOIN knowledge k ON k.key = vs.key "
                    "WHERE k.is_active ORDER BY vs.score DESC",
                    params));
        } else {
            json_object_set_new(bundle, "related", json_array());
        }
    }

    return bundle;
}
