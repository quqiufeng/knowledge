#include "compile.hpp"

#include "tokenize.hpp"

#include <jansson.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

const std::unordered_set<std::string>& base_columns() {
    static const std::unordered_set<std::string> s = {
        "id", "key", "version", "updated_at", "start_time",
        "end_time", "is_active", "is_archived",
    };
    return s;
}

struct Ctx {
    std::vector<std::string> params;
    bool has_search{false};
    std::string fti_tsq;
    const std::function<bool(const std::string&, long long&)>* resolve_key{nullptr};
};

std::string bind_param(Ctx& ctx, const std::string& text) {
    ctx.params.push_back(text);
    return "$" + std::to_string(ctx.params.size());
}

bool is_number(const json_t* v) { return json_is_integer(v) || json_is_real(v); }

std::string value_to_text(const json_t* v) {
    if (json_is_string(v)) return json_string_value(v);
    if (json_is_integer(v)) return std::to_string(json_integer_value(v));
    if (json_is_real(v)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", json_real_value(v));
        return buf;
    }
    if (json_is_true(v)) return "true";
    if (json_is_false(v)) return "false";
    if (json_is_null(v)) return "";
    throw std::runtime_error("[JSE] unsupported scalar value type");
}

bool is_path_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::vector<std::string> validate_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') {
            if (cur.empty()) throw std::runtime_error("[JSE_SECURITY] empty path segment: " + path);
            parts.push_back(cur);
            cur.clear();
        } else if (is_path_char(c)) {
            cur.push_back(c);
        } else {
            throw std::runtime_error("[JSE_SECURITY] invalid path character in: " + path);
        }
    }
    if (cur.empty()) throw std::runtime_error("[JSE_SECURITY] empty path segment: " + path);
    parts.push_back(cur);
    return parts;
}

std::string json_path_expr(const std::string& alias, const std::string& column, const std::string& path) {
    std::vector<std::string> parts = validate_path(path);
    std::string expr = alias + "." + column;
    for (size_t i = 0; i < parts.size(); ++i) {
        bool last = (i + 1 == parts.size());
        expr += last ? "->>'" : "->'";
        expr += parts[i];
        expr += "'";
    }
    return expr;
}

std::string pg_text_array(const std::vector<std::string>& items) {
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

std::string pg_numeric_array(const std::vector<std::string>& items) {
    std::string out = "{";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += items[i];
    }
    out += "}";
    return out;
}

std::string compile_node(const json_t* node, Ctx& ctx, const std::string& alias);

std::string compile_field(const std::string& expr, const json_t* meta, Ctx& ctx) {
    std::vector<std::string> clauses;

    const json_t* v = json_object_get(meta, "$eq");
    if (v) {
        if (json_is_null(v)) {
            clauses.push_back(expr + " IS NULL");
        } else if (is_number(v)) {
            clauses.push_back("(" + expr + ")::numeric = " + bind_param(ctx, value_to_text(v)));
        } else {
            clauses.push_back(expr + " = " + bind_param(ctx, value_to_text(v)));
        }
    }
    v = json_object_get(meta, "$ne");
    if (v) {
        if (json_is_null(v)) {
            clauses.push_back(expr + " IS NOT NULL");
        } else if (is_number(v)) {
            clauses.push_back("(" + expr + ")::numeric <> " + bind_param(ctx, value_to_text(v)));
        } else {
            clauses.push_back(expr + " <> " + bind_param(ctx, value_to_text(v)));
        }
    }

    const char* cmp_ops[] = {"$gt", "$gte", "$lt", "$lte"};
    const char* cmp_sql[] = {">", ">=", "<", "<="};
    for (int i = 0; i < 4; ++i) {
        v = json_object_get(meta, cmp_ops[i]);
        if (!v) continue;
        std::string lhs = is_number(v) ? "(" + expr + ")::numeric" : expr;
        clauses.push_back(lhs + " " + cmp_sql[i] + " " + bind_param(ctx, value_to_text(v)));
    }

    auto compile_set = [&](const char* key, bool negated) {
        const json_t* arr = json_object_get(meta, key);
        if (!arr) return;
        if (!json_is_array(arr)) throw std::runtime_error(std::string("[JSE] ") + key + " requires an array");
        std::vector<std::string> items;
        bool numeric = true;
        size_t n = json_array_size(arr);
        if (n == 0) numeric = false;
        for (size_t i = 0; i < n; ++i) {
            const json_t* e = json_array_get(arr, i);
            if (!is_number(e)) numeric = false;
            items.push_back(value_to_text(e));
        }
        std::string lit = numeric ? pg_numeric_array(items) : pg_text_array(items);
        std::string cast = numeric ? "::numeric[]" : "::text[]";
        std::string clause = expr + " = ANY(" + bind_param(ctx, lit) + cast + ")";
        if (negated) clause = "NOT (" + clause + ")";
        clauses.push_back(clause);
    };
    compile_set("$in", false);
    compile_set("$nin", true);

    v = json_object_get(meta, "$exists");
    if (v) {
        if (!json_is_boolean(v)) throw std::runtime_error("[JSE] $exists requires a boolean");
        clauses.push_back(expr + (json_is_true(v) ? " IS NOT NULL" : " IS NULL"));
    }

    v = json_object_get(meta, "$like");
    if (v) {
        if (!json_is_string(v)) throw std::runtime_error("[JSE] $like requires a string");
        clauses.push_back(expr + " LIKE " + bind_param(ctx, json_string_value(v)));
    }
    v = json_object_get(meta, "$ilike");
    if (v) {
        if (!json_is_string(v)) throw std::runtime_error("[JSE] $ilike requires a string");
        clauses.push_back(expr + " ILIKE " + bind_param(ctx, json_string_value(v)));
    }
    v = json_object_get(meta, "$prefix");
    if (v) {
        if (!json_is_string(v)) throw std::runtime_error("[JSE] $prefix requires a string");
        clauses.push_back(expr + " LIKE " + bind_param(ctx, std::string(json_string_value(v)) + "%"));
    }

    if (clauses.empty()) throw std::runtime_error("[JSE] $meta requires at least one operator");
    std::string out = "(";
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i) out += " AND ";
        out += clauses[i];
    }
    out += ")";
    return out;
}

std::string compile_meta(const json_t* meta, Ctx& ctx, const std::string& alias) {
    const json_t* path_v = json_object_get(meta, "path");
    if (!json_is_string(path_v)) throw std::runtime_error("[JSE] $meta requires string 'path'");
    return compile_field(json_path_expr(alias, "meta", json_string_value(path_v)), meta, ctx);
}

std::string key_id_subquery(Ctx& ctx, const std::string& key) {
    if (ctx.resolve_key) {
        long long id = 0;
        if ((*ctx.resolve_key)(key, id)) return std::to_string(id);
    }
    return "(SELECT id FROM knowledge WHERE key = " + bind_param(ctx, key) + ")";
}

bool require_object(const json_t* v, const char* what) {
    if (!json_is_object(v)) throw std::runtime_error(std::string("[JSE] ") + what + " must be an object");
    return true;
}

std::string compile_triple(const json_t* triple, Ctx& ctx, const std::string& alias) {
    std::vector<std::string> clauses;
    clauses.push_back("st.is_active");

    const json_t* dir_v = json_object_get(triple, "direction");
    std::string dir = "both";
    if (dir_v) {
        if (!json_is_string(dir_v)) throw std::runtime_error("[JSE] $triple direction must be a string");
        dir = json_string_value(dir_v);
        if (dir != "out" && dir != "in" && dir != "both")
            throw std::runtime_error("[JSE] $triple direction must be out/in/both");
    }
    if (dir == "out") clauses.push_back(alias + ".id = st.object_id");
    else if (dir == "in") clauses.push_back(alias + ".id = st.subject_id");
    else clauses.push_back("(" + alias + ".id = st.subject_id OR " + alias + ".id = st.object_id)");

    const json_t* s = json_object_get(triple, "subject");
    const json_t* p = json_object_get(triple, "predicate");
    const json_t* o = json_object_get(triple, "object");
    bool has_endpoint = false;
    if (json_is_string(s)) { clauses.push_back("st.subject_id = " + key_id_subquery(ctx, json_string_value(s))); has_endpoint = true; }
    if (json_is_string(p)) { clauses.push_back("st.predicate_id = " + key_id_subquery(ctx, json_string_value(p))); has_endpoint = true; }
    if (json_is_string(o)) { clauses.push_back("st.object_id = " + key_id_subquery(ctx, json_string_value(o))); has_endpoint = true; }
    if (!has_endpoint) throw std::runtime_error("[JSE] $triple needs at least one endpoint");
    std::string out = "EXISTS (SELECT 1 FROM statement st WHERE ";
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i) out += " AND ";
        out += clauses[i];
    }
    out += ")";
    return out;
}

std::string compile_khop(const json_t* hop, Ctx& ctx, const std::string& alias) {
    const json_t* from_v = json_object_get(hop, "from");
    const json_t* preds_v = json_object_get(hop, "predicates");
    const json_t* depth_v = json_object_get(hop, "depth");
    if (!json_is_string(from_v)) throw std::runtime_error("[JSE] $k-hop requires 'from'");
    if (!json_is_array(preds_v)) throw std::runtime_error("[JSE] $k-hop requires 'predicates' array");
    if (!json_is_integer(depth_v)) throw std::runtime_error("[JSE] $k-hop requires integer 'depth'");
    json_int_t depth = json_integer_value(depth_v);
    if (depth < 1) depth = 1;
    if (depth > 5) depth = 5;

    const json_t* dir_v = json_object_get(hop, "direction");
    std::string dir = json_is_string(dir_v) ? json_string_value(dir_v) : "out";
    std::string src = (dir == "in") ? "object_id" : "subject_id";
    std::string tgt = (dir == "in") ? "subject_id" : "object_id";

    std::vector<std::string> preds;
    size_t np = json_array_size(preds_v);
    for (size_t i = 0; i < np; ++i) {
        const json_t* e = json_array_get(preds_v, i);
        if (!json_is_string(e)) throw std::runtime_error("[JSE] $k-hop predicates must be strings");
        preds.push_back(json_string_value(e));
    }
    if (preds.empty()) throw std::runtime_error("[JSE] $k-hop requires non-empty predicates");

    std::string from_param = bind_param(ctx, json_string_value(from_v));
    std::string pred_param = bind_param(ctx, pg_text_array(preds));

    std::vector<std::string> inner = {"c.depth > 0"};
    const json_t* where = json_object_get(hop, "where");
    if (where) inner.push_back(compile_node(where, ctx, "k"));

    std::string inner_where;
    for (size_t i = 0; i < inner.size(); ++i) {
        inner_where += (i ? " AND " : "") + inner[i];
    }

    std::string sql = "(" + alias + ".id IN (WITH RECURSIVE chain AS ("
        "SELECT k.id AS current_id, 0 AS depth, ARRAY[k.id] AS path FROM knowledge k "
        "WHERE k.key = " + from_param + " AND k.is_active "
        "UNION ALL "
        "SELECT st." + tgt + ", c.depth + 1, c.path || st." + tgt + " FROM statement st "
        "JOIN chain c ON st." + src + " = c.current_id "
        "WHERE c.depth < " + std::to_string(depth) + " "
        "AND st.predicate_id IN (SELECT id FROM knowledge WHERE key = ANY(" + pred_param + "::text[])) "
        "AND st.is_active AND NOT (st." + tgt + " = ANY(c.path))"
        ") SELECT c.current_id FROM chain c JOIN knowledge k ON k.id = c.current_id "
        "WHERE " + inner_where + " LIMIT 20000))";
    return sql;
}

std::string compile_node(const json_t* node, Ctx& ctx, const std::string& alias) {
    if (!json_is_object(node)) throw std::runtime_error("[JSE] node must be an object");

    static const char* op_keys[] = {"$and", "$or", "$not", "$meta", "$key", "$fti", "$search", "$triple", "$k-hop"};
    int present = 0;
    for (const char* k : op_keys) {
        if (json_object_get(node, k)) present++;
    }
    if (present > 1) throw std::runtime_error("[JSE] a node must contain exactly one operator");

    if (const json_t* v = json_object_get(node, "$and")) {
        if (!json_is_array(v) || json_array_size(v) == 0) throw std::runtime_error("[JSE] $and requires non-empty array");
        std::string out = "(";
        for (size_t i = 0; i < json_array_size(v); ++i) {
            if (i) out += " AND ";
            out += compile_node(json_array_get(v, i), ctx, alias);
        }
        return out + ")";
    }
    if (const json_t* v = json_object_get(node, "$or")) {
        if (!json_is_array(v) || json_array_size(v) == 0) throw std::runtime_error("[JSE] $or requires non-empty array");
        std::string out = "(";
        for (size_t i = 0; i < json_array_size(v); ++i) {
            if (i) out += " OR ";
            out += compile_node(json_array_get(v, i), ctx, alias);
        }
        return out + ")";
    }
    if (const json_t* v = json_object_get(node, "$not")) {
        return "(NOT " + compile_node(v, ctx, alias) + ")";
    }
    if (const json_t* v = json_object_get(node, "$meta")) {
        require_object(v, "$meta");
        return compile_meta(v, ctx, alias);
    }
    if (const json_t* v = json_object_get(node, "$key")) {
        require_object(v, "$key");
        return compile_field(alias + ".key", v, ctx);
    }
    if (const json_t* v = json_object_get(node, "$fti")) {
        if (!json_is_string(v)) throw std::runtime_error("[JSE] $fti requires a string");
        std::vector<std::string> tokens = tokenize_code(json_string_value(v));
        if (tokens.empty()) return "(FALSE)";
        std::string tsq = build_ts_query(tokens);
        if (ctx.fti_tsq.empty()) ctx.fti_tsq = tsq;
        return "(" + alias + ".search_tsv @@ to_tsquery('simple', " + bind_param(ctx, tsq) + "))";
    }
    if (json_object_get(node, "$search")) {
        if (!ctx.has_search) throw std::runtime_error("[JSE] $search requires external vector results");
        return "(EXISTS (SELECT 1 FROM vs WHERE vs.key = " + alias + ".key))";
    }
    if (const json_t* v = json_object_get(node, "$triple")) {
        require_object(v, "$triple");
        return compile_triple(v, ctx, alias);
    }
    if (const json_t* v = json_object_get(node, "$k-hop")) {
        require_object(v, "$k-hop");
        return compile_khop(v, ctx, alias);
    }
    throw std::runtime_error("[JSE] unsupported node");
}

bool ast_has_search(const json_t* node) {
    if (!json_is_object(node)) return false;
    if (json_object_get(node, "$search")) return true;
    if (const json_t* v = json_object_get(node, "$and")) {
        for (size_t i = 0; i < json_array_size(v); ++i)
            if (ast_has_search(json_array_get(v, i))) return true;
    }
    if (const json_t* v = json_object_get(node, "$or")) {
        for (size_t i = 0; i < json_array_size(v); ++i)
            if (ast_has_search(json_array_get(v, i))) return true;
    }
    if (const json_t* v = json_object_get(node, "$not")) return ast_has_search(v);
    if (const json_t* v = json_object_get(node, "$k-hop")) {
        const json_t* w = json_object_get(v, "where");
        if (w) return ast_has_search(w);
    }
    return false;
}

std::string project_expr(const std::string& alias, const std::string& path) {
    if (path.rfind("meta.", 0) == 0) return json_path_expr(alias, "meta", path.substr(5));
    if (path.rfind("content.", 0) == 0) return json_path_expr(alias, "content", path.substr(8));
    if (base_columns().count(path)) return alias + "." + path;
    throw std::runtime_error("[JSE_SECURITY] invalid projection path: " + path);
}

std::string order_expr(Ctx& ctx, const std::string& alias, const std::string& key, const std::string& dir) {
    std::string upper = dir == "asc" ? "ASC" : "DESC";
    if (key == "$search_score") return "(SELECT score FROM vs WHERE vs.key = " + alias + ".key) " + upper;
    if (key == "$fti_rank") {
        if (ctx.fti_tsq.empty()) throw std::runtime_error("[JSE] $order $fti_rank requires an $fti node");
        return "ts_rank(" + alias + ".search_tsv, to_tsquery('simple', " + bind_param(ctx, ctx.fti_tsq) + ")) " + upper;
    }
    if (key == "$in_degree")
        return "(SELECT count(*) FROM statement st WHERE st.object_id = " + alias + ".id AND st.is_active) " + upper;
    if (key == "$out_degree")
        return "(SELECT count(*) FROM statement st WHERE st.subject_id = " + alias + ".id AND st.is_active) " + upper;
    if (key.rfind("meta.", 0) == 0) return json_path_expr(alias, "meta", key.substr(5)) + " " + upper;
    if (base_columns().count(key)) return alias + "." + key + " " + upper;
    throw std::runtime_error("[JSE_SECURITY] invalid order key: " + key);
}

}  // namespace

CompiledQuery compile_query(const json_t* query, const CompileOptions& opts) {
    if (!json_is_object(query)) throw std::runtime_error("[JSE] query must be an object");
    const json_t* where = json_object_get(query, "$where");
    if (!where) throw std::runtime_error("[JSE] query requires $where");

    const std::vector<VectorHit>& vectors = opts.vectors;
    Ctx ctx;
    ctx.resolve_key = &opts.resolve_key;
    std::string alias = opts.alias;
    std::string cte;

    if (ast_has_search(where)) {
        if (vectors.empty()) throw std::runtime_error("[JSE] $search requires vectors from the external engine");
        std::string rows;
        for (size_t i = 0; i < vectors.size(); ++i) {
            if (i) rows += ", ";
            rows += "(" + bind_param(ctx, vectors[i].key) + "::text, " + bind_param(ctx, std::to_string(vectors[i].score)) + "::double precision)";
        }
        cte = "WITH vs(key, score) AS (VALUES " + rows + ") ";
        ctx.has_search = true;
    }

    std::string where_sql = compile_node(where, ctx, alias);

    json_int_t out_limit = 100;
    if (json_is_integer(json_object_get(query, "$limit"))) {
        out_limit = json_integer_value(json_object_get(query, "$limit"));
        if (out_limit < 1) out_limit = 1;
        if (out_limit > 1000) out_limit = 1000;
    }

    const json_t* group_v = json_object_get(query, "$group_by");
    if (json_is_string(group_v)) {
        std::string gexpr = project_expr(alias, json_string_value(group_v));
        CompiledQuery out;
        out.sql = cte + "SELECT " + gexpr + " AS \"group\", count(*) AS count FROM knowledge " + alias +
                  " WHERE " + where_sql + " GROUP BY 1 ORDER BY count DESC LIMIT " + std::to_string(out_limit);
        out.params = ctx.params;
        return out;
    }

    if (json_is_true(json_object_get(query, "$count"))) {
        CompiledQuery out;
        out.sql = cte + "SELECT count(*) AS count FROM knowledge " + alias + " WHERE " + where_sql;
        out.params = ctx.params;
        return out;
    }

    std::string projection;
    const json_t* proj = json_object_get(query, "$project");
    if (json_is_array(proj) && json_array_size(proj) > 0) {
        for (size_t i = 0; i < json_array_size(proj); ++i) {
            const json_t* p = json_array_get(proj, i);
            if (!json_is_string(p)) throw std::runtime_error("[JSE] $project entries must be strings");
            std::string path = json_string_value(p);
            if (i) projection += ", ";
            projection += project_expr(alias, path) + " AS \"" + path + "\"";
        }
    } else {
        projection = alias + ".id, " + alias + ".key, " + alias + ".meta";
    }

    std::string order_sql;
    const json_t* order = json_object_get(query, "$order");
    if (json_is_object(order) && json_object_size(order) > 0) {
        order_sql = " ORDER BY ";
        size_t idx = 0;
        void* iter = json_object_iter(const_cast<json_t*>(order));
        while (iter) {
            const char* k = json_object_iter_key(iter);
            const json_t* val = json_object_iter_value(iter);
            if (idx++) order_sql += ", ";
            if (!json_is_string(val)) throw std::runtime_error("[JSE] $order values must be 'asc'/'desc'");
            if (std::string(k) == "$search_score" && !ctx.has_search)
                throw std::runtime_error("[JSE] $order $search_score requires a $search node");
            order_sql += order_expr(ctx, alias, k, json_string_value(val));
            iter = json_object_iter_next(const_cast<json_t*>(order), iter);
        }
    }

    json_int_t limit = 100;
    const json_t* limit_v = json_object_get(query, "$limit");
    if (json_is_integer(limit_v)) {
        limit = json_integer_value(limit_v);
        if (limit < 1) limit = 1;
        if (limit > 1000) limit = 1000;
    }
    std::string offset_sql;
    const json_t* offset_v = json_object_get(query, "$offset");
    if (json_is_integer(offset_v) && json_integer_value(offset_v) > 0) {
        offset_sql = " OFFSET " + std::to_string(json_integer_value(offset_v));
    }

    CompiledQuery out;
    out.sql = cte + "SELECT " + projection + " FROM knowledge " + alias + " WHERE " + where_sql +
              order_sql + " LIMIT " + std::to_string(limit) + offset_sql;
    out.params = ctx.params;
    return out;
}
