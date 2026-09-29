#include "api.hpp"
#include "books.hpp"
#include "compile.hpp"
#include "db.hpp"
#include "importer.hpp"
#include "action.hpp"
#include "embed.hpp"
#include "export.hpp"
#include "memory.hpp"
#include "records.hpp"
#include "server.hpp"
#include "spec.hpp"
#include "util.hpp"
#include "vector.hpp"
#include "write.hpp"

#include <cstdio>
#include <filesystem>
#include "tokenize.hpp"

#include <jansson.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
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

void seed(Db& db) {
    Db::Tx tx(db);
    const std::string p_calls = "/pred/calls";
    const std::string p_impl = "/pred/implements";
    auto pred = [&](const std::string& key, const std::string& name) {
        kwrite::upsert_entry(db, key, std::string(R"({"category":"predicate","name":")") + name + R"("})",
                             "{}", name);
    };
    pred(p_calls, "calls");
    pred(p_impl, "implements");

    auto func = [&](const std::string& sym, const std::string& file, int line, const std::string& code) {
        std::string key = "/code/local/linux/mm/" + file + "/" + sym;
        std::string meta = std::string(R"({"kind":"function","lang":"c","file":")") + file +
                           R"(","line":)" + std::to_string(line) + R"(,"symbol":")" + sym + R"("})";
        std::string content = std::string(R"({"code":")") + code + R"("})";
        std::vector<std::string> tokens = tokenize_code(code + " " + sym + " " + meta);
        kwrite::upsert_entry(db, key, meta, content, to_tsvector_text(tokens));
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

    kwrite::link_edge(db, slowpath, p_calls, prepare, true);
    kwrite::link_edge(db, slowpath, p_calls, may_oom, true);
    kwrite::link_edge(db, alloc, p_calls, slowpath, true);
    tx.commit();
}

void print_compiled(const CompiledQuery& cq) {
    json_t* out = json_object();
    json_object_set_new(out, "sql", json_string(cq.sql.c_str()));
    json_t* params = json_array();
    for (const auto& p : cq.params) json_array_append_new(params, json_string(p.c_str()));
    json_object_set_new(out, "params", params);
    std::cout << json_dump(out, JSON_INDENT(2)) << std::endl;
    json_decref(out);
}

void run_query(Db& db, const json_t* query, const std::vector<VectorHit>& vectors, bool execute) {
    std::unordered_map<std::string, long long> cache;
    CompiledQuery cq = compile_query(query, make_options(db, vectors, cache));
    if (!execute) {
        print_compiled(cq);
        return;
    }
    json_t* rows = db.query_json(cq.sql, cq.params);
    std::cout << json_dump(rows, JSON_INDENT(2)) << std::endl;
    json_decref(rows);
}

// Emit {"<field>":"<value>"} safely (keys/values may contain quotes).
void print_kv(const char* field, const std::string& value) {
    json_t* o = json_object();
    json_object_set_new(o, field, json_string(value.c_str()));
    std::cout << json_dump(o) << std::endl;
    json_decref(o);
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
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
                 "           [--limit N] [--skip-callgraph] [--skip-dataflow] [--fanout]\n"
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
                 "  link     <subject> <predicate> <object> [--agent id]\n"
                 "  put      --key K [--meta <json>] [--content <json>] [--category C] [--text T] [--agent id]\n"
                 "  forget   <key> [--force] [--agent id]\n"
                 "  restore  <key> [--agent id]\n"
                 "  spec-set <category> <spec-json>   # schema-as-data; validated by put/import-records\n"
                 "\n"
                 "export (JSONL to stdout):\n"
                 "  export --preset edges   [--predicate /pred/calls] [--key-prefix P] [--limit N]\n"
                 "  export --preset context [--key-prefix P] [--limit N] [--depth N] [--no-content]\n"
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
                    name == "no-content" || name == "ro" || name == "force" || name == "fanout" || name == "exclude-headers") {
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
            kutil::json_ptr query(load_query(read_stdin()));
            std::vector<VectorHit> vectors = parse_vectors(vectors_json);
            if (cmd == "query") {
                Db db(default_conninfo());
                run_query(db, query.get(), vectors, true);
                return 0;
            }
            // compile: the database is optional (it only powers key->id resolution).
            std::unique_ptr<Db> db;
            try {
                db = std::make_unique<Db>(default_conninfo());
            } catch (const std::exception& e) {
                std::cerr << "[COMPILE] " << e.what() << "; key->id resolution disabled\n";
            }
            std::unordered_map<std::string, long long> cache;
            CompileOptions copts;
            copts.vectors = vectors;
            if (db) copts.resolve_key = make_resolver(*db, cache);
            print_compiled(compile_query(query.get(), copts));
            return 0;
        }
        if (cmd == "search") {
            if (pos.empty()) return usage();
            int k = opt_int("k", 20);
            std::string scalar = opts.count("where") ? opts["where"] : "";
            std::string embed_cmd = opts.count("embed-cmd") ? opts["embed-cmd"] : "";
            // Vector source is chosen inside api_search: --vector-cmd provider,
            // otherwise embed (KNOWLEDGE_EMBED_CMD) + pgvector, else lexical only.
            Db db(default_conninfo());
            json_t* r = api_search(db, pos[0], k, vector_cmd, scalar, {}, embed_cmd);
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
            json_t* r = api_context(db, pos[0], depth, k, pred, vector_cmd, no_code, max_bytes, opts.count("exclude-headers") > 0);
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
            io.fanout = opts.count("fanout") > 0;
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
            EmbedResult ev = import_vectors(db, eo);
            json_t* o = json_object();
            json_object_set_new(o, "vectors", json_integer(ev.matched));
            json_object_set_new(o, "staged", json_integer(ev.staged));
            std::cout << json_dump(o) << std::endl;
            json_decref(o);
            return 0;
        }
        if (cmd == "qsearch") {
            if (pos.empty()) return usage();
            int k = opt_int("k", 10);
            std::string where = opts.count("where") ? opts["where"] : "";
            std::string embed_cmd = opts.count("embed-cmd") ? opts["embed-cmd"] : "";
            std::vector<float> qvec = embed_text(pos[0], embed_cmd);
            json_t* varr = json_array();
            for (float f : qvec) json_array_append_new(varr, json_real(f));
            json_t* knn = json_object();
            json_object_set_new(knn, "vector", varr);
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
            ro.category = opts.count("category") ? opts["category"] : "";
            Db db(default_conninfo());
            RecordsResult rr = import_records(db, ro);
            json_t* o = json_object();
            json_object_set_new(o, "imported", json_integer(rr.imported));
            json_object_set_new(o, "skipped", json_integer(rr.skipped));
            std::cout << json_dump(o) << std::endl;
            json_decref(o);
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
            print_kv("key", key);
            return 0;
        }
        if (cmd == "fact") {
            Db db(default_conninfo());
            std::string key = memory_fact(db, opts.count("agent") ? opts["agent"] : "",
                                          opts.count("topic") ? opts["topic"] : "",
                                          opts.count("value") ? opts["value"] : "null",
                                          opts.count("session") ? opts["session"] : "");
            print_kv("key", key);
            return 0;
        }
        if (cmd == "link") {
            if (pos.size() < 3) return usage();
            Db db(default_conninfo());
            long c = memory_link(db, opts.count("agent") ? opts["agent"] : "", pos[0], pos[1], pos[2]);
            std::cout << "{\"matches\":" << c << "}\n";
            return 0;
        }
        if (cmd == "forget") {
            if (pos.empty()) return usage();
            Db db(default_conninfo());
            bool changed = action_forget(db, opts.count("agent") ? opts["agent"] : "", pos[0],
                                         opts.count("force") > 0);
            std::cout << "{\"forgotten\":" << (changed ? "true" : "false") << "}\n";
            return 0;
        }
        if (cmd == "restore") {
            if (pos.empty()) return usage();
            Db db(default_conninfo());
            bool changed = action_restore(db, opts.count("agent") ? opts["agent"] : "", pos[0]);
            std::cout << "{\"restored\":" << (changed ? "true" : "false") << "}\n";
            return 0;
        }
        if (cmd == "put") {
            Db db(default_conninfo());
            std::string key = action_put(
                db, opts.count("agent") ? opts["agent"] : "", opts.count("key") ? opts["key"] : "",
                opts.count("meta") ? opts["meta"] : "{}", opts.count("content") ? opts["content"] : "{}",
                opts.count("category") ? opts["category"] : "", opts.count("text") ? opts["text"] : "");
            print_kv("key", key);
            return 0;
        }
        if (cmd == "export") {
            ExportOptions eo;
            eo.preset = opts.count("preset") ? opts["preset"] : "edges";
            eo.predicate = opts.count("predicate") ? opts["predicate"] : "";
            eo.key_prefix = opts.count("key-prefix") ? opts["key-prefix"] : "";
            eo.limit = opts.count("limit") ? std::atol(opts["limit"].c_str()) : 1000;
            eo.depth = opt_int("depth", 1);
            eo.no_content = opts.count("no-content") > 0;
            Db db(default_conninfo());
            long n = export_corpus(db, eo);
            std::cerr << "[EXPORT] " << n << " records\n";
            return 0;
        }
        if (cmd == "spec-set") {
            if (pos.size() < 2) return usage();
            Db db(default_conninfo());
            spec_set(db, pos[0], pos[1]);
            print_kv("spec", "/spec/" + pos[0]);
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
        json_decref(err);
        return 1;
    }
}
