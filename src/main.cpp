#include "api.hpp"
#include "books.hpp"
#include "compile.hpp"
#include "db.hpp"
#include "importer.hpp"
#include "embed.hpp"
#include "memory.hpp"
#include "records.hpp"
#include "server.hpp"

#include <cstdio>
#include <filesystem>
#include "tokenize.hpp"

#include <jansson.h>

#include <cstdlib>
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

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string run_cmd(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw std::runtime_error("[CMD] popen failed");
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    if (rc != 0 && out.empty()) throw std::runtime_error("[CMD] command failed: " + cmd);
    return out;
}

std::vector<float> parse_float_vector(const std::string& text) {
    std::vector<float> out;
    json_t* arr = json_loads(text.c_str(), 0, nullptr);
    if (json_is_array(arr)) {
        for (size_t i = 0; i < json_array_size(arr); ++i) {
            const json_t* e = json_array_get(arr, i);
            if (json_is_number(e)) out.push_back(static_cast<float>(json_number_value(e)));
        }
    }
    if (arr) json_decref(arr);
    return out;
}

void split_csv(const std::string& s, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

int usage() {
    std::cerr << "usage: knowledge <command> [options]\n"
                 "  tokenize <text>\n"
                 "  compile  [--vectors <json>]                 (JSE from stdin)\n"
                 "  query    [--vectors <json>]                 (JSE from stdin, runs against PG)\n"
                 "  search   <query> [--k N] [--where <jse>] [--vector-cmd <cmd>] [--embed-cmd <cmd>]\n"
                 "           (default: embed query + pgvector; --vector-cmd uses the external engine)\n"
                 "  context  <key>   [--depth N] [--k N] [--predicate <key>]\n"
                 "                   [--no-content] [--max-code-bytes N] [--vector-cmd <cmd>]\n"
                 "  import   --analysis-dir <dir> --project <p> --root <r>\n"
                 "           [--limit N] [--skip-callgraph] [--skip-dataflow]\n"
                 "  import-books [--books-dir <dir>] [--book <name>] [--limit N]\n"
                 "  import-records <file> [--format jsonl|csv] [--key-field key] [--kind K]\n"
                 "                 [--text-field F] [--prefix /data/]\n"
                 "  import-vectors --project <p> --root <r> (--analysis-dir <d> | --bin <f> --meta <f>)\n"
                 "  qsearch  <query> [--k N] [--where <jse>] [--embed-cmd <cmd>]\n"
                 "  serve    [--port N] [--bind <addr>] [--pool N] [--ro] [--vector-cmd <cmd>]\n"
                 "\n"
                 "agent memory (local actions; write role):\n"
                 "  remember --agent <id> --text <t> [--session s] [--about k1,k2] [--tag a,b]\n"
                 "  fact     --agent <id> --topic <t> --value <json> [--session s]\n"
                 "  link     <subject> <predicate> <object>\n"
                 "  forget   <key> [--force]\n"
                 "\n"
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
                if (name == "skip-callgraph" || name == "skip-dataflow" || name == "no-code" ||
                    name == "no-content" || name == "ro" || name == "force") {
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
        if (cmd == "search") {
            if (pos.empty()) return usage();
            int k = opt_int("k", 20);
            std::string scalar = opts.count("where") ? opts["where"] : "";
            std::vector<float> qvec;
            if (!opts.count("vector-cmd")) {
                // Default: embed the query and use pgvector for vector candidates.
                std::string embed_cmd = opts.count("embed-cmd")
                                            ? opts["embed-cmd"]
                                            : env_or("KNOWLEDGE_EMBED_CMD", "./tools/embed_query.sh");
                try {
                    qvec = parse_float_vector(run_cmd(embed_cmd + " " + sh_quote(pos[0])));
                } catch (const std::exception&) {
                    qvec.clear();
                }
            }
            Db db(default_conninfo());
            json_t* r = api_search(db, pos[0], k, vector_cmd, scalar, qvec);
            std::cout << json_dump(r, JSON_INDENT(2)) << std::endl;
            json_decref(r);
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
            json_t* r = api_context(db, pos[0], depth, k, pred, vector_cmd, no_code, max_bytes);
            std::cout << json_dump(r, JSON_INDENT(2)) << std::endl;
            json_decref(r);
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
        if (cmd == "serve") {
            ServerOptions so;
            so.port = opt_int("port", 8931);
            so.bind = opts.count("bind") ? opts["bind"] : "127.0.0.1";
            so.pool = opt_int("pool", 4);
            so.read_only = opts.count("ro") > 0;
            so.conninfo = so.read_only
                              ? env_or("KNOWLEDGE_RO_URL",
                                       "postgresql://knowledge_ro:knowledge_ro@127.0.0.1:5432/knowledge")
                              : default_conninfo();
            so.vector_cmd = vector_cmd;
            return run_server(so);
        }
        if (cmd == "import-vectors") {
            EmbedOptions eo;
            eo.project = opts.count("project") ? opts["project"] : "";
            eo.root = opts.count("root") ? opts["root"] : "";
            eo.limit = opts.count("limit") ? std::atol(opts["limit"].c_str()) : 0;
            if (opts.count("analysis-dir")) {
                std::string dir = opts["analysis-dir"];
                eo.meta_file = dir + "/chunks_meta.jsonl";
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(dir + "/vectors", ec)) {
                    if (e.path().extension() == ".bin") {
                        eo.bin_file = e.path().string();
                        break;
                    }
                }
            }
            if (opts.count("bin")) eo.bin_file = opts["bin"];
            if (opts.count("meta")) eo.meta_file = opts["meta"];
            Db db(default_conninfo());
            long n = import_vectors(db, eo);
            std::cout << "{\"vectors\":" << n << "}\n";
            return 0;
        }
        if (cmd == "qsearch") {
            if (pos.empty()) return usage();
            int k = opt_int("k", 10);
            std::string where = opts.count("where") ? opts["where"] : "";
            std::string embed_cmd =
                opts.count("embed-cmd") ? opts["embed-cmd"] : env_or("KNOWLEDGE_EMBED_CMD", "./tools/embed_query.sh");
            std::string vec = run_cmd(embed_cmd + " " + sh_quote(pos[0]));
            json_t* vecarr = json_loads(vec.c_str(), 0, nullptr);
            if (!json_is_array(vecarr)) {
                if (vecarr) json_decref(vecarr);
                throw std::runtime_error("[QSEARCH] embedding failed (no vector)");
            }
            json_t* knn = json_object();
            json_object_set_new(knn, "vector", json_incref(vecarr));
            json_object_set_new(knn, "k", json_integer(k));
            json_t* knn_node = json_object();
            json_object_set_new(knn_node, "$knn", knn);
            json_t* and_arr = json_array();
            json_array_append_new(and_arr, knn_node);
            if (!where.empty()) {
                json_t* extra = json_loads(where.c_str(), 0, nullptr);
                if (!extra) throw std::runtime_error("[QSEARCH] invalid --where JSE");
                json_array_append_new(and_arr, extra);
            }
            json_t* inner = json_object();
            json_object_set_new(inner, "$and", and_arr);
            json_t* q = json_object();
            json_object_set_new(q, "$where", inner);
            json_t* proj = json_array();
            for (const char* p : {"key", "meta.symbol", "meta.file", "meta.line", "meta.signature"})
                json_array_append_new(proj, json_string(p));
            json_object_set_new(q, "$project", proj);
            json_object_set_new(q, "$limit", json_integer(k));
            Db db(default_conninfo());
            run_query(db, q, {}, true);
            json_decref(q);
            json_decref(vecarr);
            return 0;
        }
        if (cmd == "import-records") {
            if (pos.empty()) return usage();
            RecordsOptions ro;
            ro.file = pos[0];
            ro.format = opts.count("format") ? opts["format"] : "";
            ro.key_field = opts.count("key-field") ? opts["key-field"] : "key";
            ro.kind = opts.count("kind") ? opts["kind"] : "record";
            ro.text_field = opts.count("text-field") ? opts["text-field"] : "";
            ro.prefix = opts.count("prefix") ? opts["prefix"] : "";
            Db db(default_conninfo());
            long n = import_records(db, ro);
            std::cout << "{\"imported\":" << n << "}\n";
            return 0;
        }
        if (cmd == "remember") {
            std::vector<std::string> about, tags;
            if (opts.count("about")) split_csv(opts["about"], about);
            if (opts.count("tag")) split_csv(opts["tag"], tags);
            Db db(default_conninfo());
            std::string key = memory_remember(db, opts.count("agent") ? opts["agent"] : "",
                                              opts.count("session") ? opts["session"] : "",
                                              opts.count("text") ? opts["text"] : "", about, tags);
            std::cout << "{\"key\":\"" << key << "\"}\n";
            return 0;
        }
        if (cmd == "fact") {
            Db db(default_conninfo());
            std::string key = memory_fact(db, opts.count("agent") ? opts["agent"] : "",
                                          opts.count("topic") ? opts["topic"] : "",
                                          opts.count("value") ? opts["value"] : "null",
                                          opts.count("session") ? opts["session"] : "");
            std::cout << "{\"key\":\"" << key << "\"}\n";
            return 0;
        }
        if (cmd == "link") {
            if (pos.size() < 3) return usage();
            Db db(default_conninfo());
            long c = memory_link(db, pos[0], pos[1], pos[2]);
            std::cout << "{\"matches\":" << c << "}\n";
            return 0;
        }
        if (cmd == "forget") {
            if (pos.empty()) return usage();
            Db db(default_conninfo());
            bool changed = memory_forget(db, pos[0], opts.count("force") > 0);
            std::cout << "{\"forgotten\":" << (changed ? "true" : "false") << "}\n";
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
