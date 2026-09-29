#include "books.hpp"
#include "compile.hpp"
#include "db.hpp"
#include "importer.hpp"
#include "tokenize.hpp"
#include "vector.hpp"

#include <jansson.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string read_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

std::string json_dump(const json_t* v, size_t flags = JSON_COMPACT) {
    char* s = json_dumps(v, flags);
    std::string out = s ? s : "";
    if (s) free(s);
    return out;
}

std::vector<VectorHit> parse_vectors(const std::string& text) {
    std::vector<VectorHit> out;
    if (text.empty()) return out;
    json_error_t err;
    json_t* arr = json_loads(text.c_str(), 0, &err);
    if (!arr) throw std::runtime_error(std::string("[INPUT] invalid vectors JSON: ") + err.text);
    if (!json_is_array(arr)) {
        json_decref(arr);
        throw std::runtime_error("[INPUT] vectors must be a JSON array");
    }
    for (size_t i = 0; i < json_array_size(arr); ++i) {
        const json_t* e = json_array_get(arr, i);
        VectorHit hit;
        const json_t* k = json_object_get(e, "key");
        if (!json_is_string(k)) {
            json_decref(arr);
            throw std::runtime_error("[INPUT] vector hit requires string 'key'");
        }
        hit.key = json_string_value(k);
        const json_t* sc = json_object_get(e, "score");
        if (json_is_number(sc)) hit.score = json_number_value(sc);
        out.push_back(hit);
    }
    json_decref(arr);
    return out;
}

json_t* load_query(const std::string& text) {
    json_error_t err;
    json_t* q = json_loads(text.c_str(), 0, &err);
    if (!q) throw std::runtime_error(std::string("[INPUT] invalid JSE JSON: ") + err.text);
    return q;
}

void upsert_entry(Db& db, const std::string& key, const std::string& meta_json,
                  const std::string& content_json, const std::string& search_text) {
    std::vector<std::string> tokens = tokenize_code(search_text + " " + meta_json);
    std::string tsv = to_tsvector_text(tokens);
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "VALUES ($1, $2::jsonb, $3::jsonb, to_tsvector('simple', $4)) "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()",
        {key, meta_json, content_json, tsv});
}

void link_triple(Db& db, const std::string& s, const std::string& p, const std::string& o) {
    db.exec(
        "INSERT INTO statement (subject_id, predicate_id, object_id) "
        "SELECT s.id, p.id, o.id FROM knowledge s, knowledge p, knowledge o "
        "WHERE s.key = $1 AND p.key = $2 AND o.key = $3 ON CONFLICT DO NOTHING",
        {s, p, o});
}

void seed(Db& db) {
    const std::string p_calls = "/pred/calls";
    const std::string p_impl = "/pred/implements";
    upsert_entry(db, p_calls, R"({"category":"predicate","name":"calls"})", "{}", "calls");
    upsert_entry(db, p_impl, R"({"category":"predicate","name":"implements"})", "{}", "implements");

    auto func = [&](const std::string& sym, const std::string& file, int line, const std::string& code) {
        std::string key = "/code/local/linux/mm/" + file + "/" + sym;
        std::string meta = std::string(R"({"kind":"function","lang":"c","file":")") + file +
                           R"(","line":)" + std::to_string(line) + R"(,"symbol":")" + sym + R"("})";
        std::string content = std::string(R"({"code":")") + code + R"("})";
        upsert_entry(db, key, meta, content, code + " " + sym);
        return key;
    };

    std::string slowpath = func("__alloc_pages_slowpath", "page_alloc.c", 312,
                                "static struct page *__alloc_pages_slowpath(gfp_t gfp_mask, unsigned int order) { prepare_alloc_pages(); __alloc_pages_may_oom(); }");
    std::string prepare = func("prepare_alloc_pages", "page_alloc.c", 412,
                               "static inline bool prepare_alloc_pages(gfp_t gfp_mask, unsigned int order) {}");
    std::string may_oom = func("__alloc_pages_may_oom", "page_alloc.c", 520,
                               "static struct page *__alloc_pages_may_oom(gfp_t gfp_mask, unsigned int order) {}");
    std::string alloc = func("alloc_pages", "page_alloc.c", 610,
                             "struct page *alloc_pages(gfp_t gfp, unsigned int order) { return __alloc_pages_slowpath(); }");
    func("zmalloc", "zmalloc.c", 100, "void *zmalloc(size_t size) { void *ptr = malloc(size + PREFIX_SIZE); }");

    link_triple(db, slowpath, p_calls, prepare);
    link_triple(db, slowpath, p_calls, may_oom);
    link_triple(db, alloc, p_calls, slowpath);
}

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

void run_query(Db& db, const json_t* query, const std::vector<VectorHit>& vectors, bool execute) {
    std::unordered_map<std::string, long long> cache;
    CompiledQuery cq = compile_query(query, make_options(db, vectors, cache));
    if (!execute) {
        json_t* out = json_object();
        json_object_set_new(out, "sql", json_string(cq.sql.c_str()));
        json_t* params = json_array();
        for (const auto& p : cq.params) json_array_append_new(params, json_string(p.c_str()));
        json_object_set_new(out, "params", params);
        std::cout << json_dump(out, JSON_INDENT(2)) << std::endl;
        json_decref(out);
        return;
    }
    json_t* rows = db.query_json(cq.sql, cq.params);
    std::cout << json_dump(rows, JSON_INDENT(2)) << std::endl;
    json_decref(rows);
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

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

void cmd_search(Db& db, const std::string& query, int k, const std::string& vector_cmd,
                const std::string& scalar_json) {
    std::vector<VectorHit> hits = run_vector_provider(vector_cmd, query, k);

    json_error_t err;
    std::unordered_map<std::string, long long> cache;

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

    const double RRF_K = 60.0;
    std::unordered_map<std::string, double> rrf;
    std::unordered_map<std::string, int> vrank, lrank;
    std::unordered_map<std::string, double> vscore;
    for (size_t i = 0; i < hits.size(); ++i) {
        rrf[hits[i].key] += 1.0 / (RRF_K + static_cast<double>(i + 1));
        vrank[hits[i].key] = static_cast<int>(i + 1);
        vscore[hits[i].key] = hits[i].score;
    }
    for (size_t i = 0; i < lex_keys.size(); ++i) {
        const std::string& key = lex_keys[i];
        rrf[key] += 1.0 / (RRF_K + static_cast<double>(i + 1));
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
        std::cout << json_dump(out, JSON_INDENT(2)) << std::endl;
        json_decref(out);
        return;
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

    json_object_set_new(out, "source", json_string(vector_cmd.empty() ? "pg+fti" : "vector+fti"));
    json_object_set_new(out, "results", rows);
    std::cout << json_dump(out, JSON_INDENT(2)) << std::endl;
    json_decref(out);
    json_decref(body);
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

void cmd_context(Db& db, const std::string& key, int depth, int k, const std::string& pred_key,
                 const std::string& vector_cmd, bool no_code, long max_code_bytes) {
    json_t* bundle = json_object();
    json_object_set_new(bundle, "key", json_string(key.c_str()));

    json_t* defs = db.query_json(
        "SELECT key, meta, content, version, updated_at FROM knowledge WHERE key = $1 AND is_active", {key});
    if (json_array_size(defs) == 0) {
        json_decref(defs);
        json_decref(bundle);
        throw std::runtime_error("[CONTEXT] key not found or inactive: " + key);
    }
    trim_definition(defs, no_code, max_code_bytes);
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

    std::cout << json_dump(bundle, JSON_INDENT(2)) << std::endl;
    json_decref(bundle);
}

int usage() {
    std::cerr << "usage: knowledge <command> [options]\n"
                 "  tokenize <text>\n"
                 "  compile  [--vectors <json>]                 (JSE from stdin)\n"
                 "  query    [--vectors <json>]                 (JSE from stdin, runs against PG)\n"
                 "  search   <query> [--k N] [--where <jse>] [--vector-cmd <cmd>]\n"
                 "  context  <key>   [--depth N] [--k N] [--predicate <key>]\n"
                 "                   [--no-content] [--max-code-bytes N] [--vector-cmd <cmd>]\n"
                 "  import   --analysis-dir <dir> --project <p> --root <r>\n"
                 "           [--limit N] [--skip-callgraph] [--skip-dataflow]\n"
                 "  import-books [--books-dir <dir>] [--book <name>] [--limit N]\n"
                 "  seed\n"
                 "  version\n"
                 "\n"
                 "env: DATABASE_URL, KNOWLEDGE_VECTOR_CMD\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) return usage();
        std::string cmd = argv[1];

        if (cmd == "version") {
            std::cout << "knowledge 0.1.0 (C++/libpq/jansson)\n";
            return 0;
        }
        if (cmd == "tokenize") {
            std::string text = argc > 2 ? argv[2] : read_stdin();
            auto tokens = tokenize_code(text);
            json_t* arr = json_array();
            for (const auto& t : tokens) json_array_append_new(arr, json_string(t.c_str()));
            std::cout << json_dump(arr) << std::endl;
            json_decref(arr);
            return 0;
        }

        std::map<std::string, std::string> opts;
        std::vector<std::string> pos;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                std::string name = a.substr(2);
                if (name == "skip-callgraph" || name == "skip-dataflow" || name == "no-code" || name == "no-content") {
                    opts[name] = "true";
                } else if (i + 1 < argc) {
                    opts[name] = argv[++i];
                }
            } else {
                pos.push_back(a);
            }
        }
        auto opt_int = [&](const char* name, int def) {
            auto it = opts.find(name);
            return it == opts.end() ? def : std::atoi(it->second.c_str());
        };
        std::string vector_cmd = opts.count("vector-cmd")
                                     ? opts["vector-cmd"]
                                     : env_or("KNOWLEDGE_VECTOR_CMD", "");

        if (cmd == "compile" || cmd == "query") {
            std::string vectors_json = opts.count("vectors") ? opts["vectors"] : "";
            json_t* query = load_query(read_stdin());
            std::vector<VectorHit> vectors = parse_vectors(vectors_json);
            Db db(default_conninfo());
            run_query(db, query, vectors, cmd == "query");
            json_decref(query);
            return 0;
        }
        if (cmd == "import") {
            ImportOptions io;
            io.analysis_dir = opts.count("analysis-dir") ? opts["analysis-dir"] : "";
            io.project = opts.count("project") ? opts["project"] : "";
            io.root = opts.count("root") ? opts["root"] : "";
            io.limit = opts.count("limit") ? std::atol(opts["limit"].c_str()) : 0;
            io.skip_callgraph = opts.count("skip-callgraph") > 0;
            io.skip_dataflow = opts.count("skip-dataflow") > 0;
            Db db(default_conninfo());
            import_analysis(db, io);
            return 0;
        }
        if (cmd == "import-books") {
            BooksOptions bo;
            bo.books_dir = opts.count("books-dir") ? opts["books-dir"] : "/opt/books";
            bo.only_book = opts.count("book") ? opts["book"] : "";
            bo.limit = opts.count("limit") ? std::atol(opts["limit"].c_str()) : 0;
            Db db(default_conninfo());
            import_books(db, bo);
            return 0;
        }
        if (cmd == "search") {
            if (pos.empty()) return usage();
            int k = opt_int("k", 20);
            std::string scalar = opts.count("where") ? opts["where"] : "";
            Db db(default_conninfo());
            cmd_search(db, pos[0], k, vector_cmd, scalar);
            return 0;
        }
        if (cmd == "context") {
            if (pos.empty()) return usage();
            int depth = opt_int("depth", 2);
            int k = opt_int("k", 10);
            std::string pred = opts.count("predicate") ? opts["predicate"] : "/pred/calls";
            bool no_code = opts.count("no-code") > 0 || opts.count("no-content") > 0;
            long max_bytes = opts.count("max-code-bytes") ? std::atol(opts["max-code-bytes"].c_str()) : 0;
            Db db(default_conninfo());
            cmd_context(db, pos[0], depth, k, pred, vector_cmd, no_code, max_bytes);
            return 0;
        }
        if (cmd == "seed") {
            Db db(default_conninfo());
            seed(db);
            std::cout << "seeded\n";
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        json_t* err = json_object();
        json_object_set_new(err, "error", json_string(e.what()));
        std::cerr << json_dump(err) << std::endl;
        return 1;
    }
}
