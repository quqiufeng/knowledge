#include "importer.hpp"

#include "tokenize.hpp"
#include "util.hpp"

#include <jansson.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace kutil;

constexpr size_t FLUSH_BYTES = 8 * 1024 * 1024;

const char* P_CALLS = "/pred/calls";
const char* P_DEFINES = "/pred/defines";
const char* P_ASSIGNS = "/pred/assigns";
const char* P_USES = "/pred/uses";

std::string make_key(const std::string& project, const std::string& rel, const std::string& name) {
    return "/code/local/" + project + "/" + rel + "/" + name;
}

inline uint64_t fnv1a(const char* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint64_t hash64(const std::string& s) { return fnv1a(s.data(), s.size()); }

inline uint64_t edge_hash(const std::string& s, const std::string& p, const std::string& o) {
    uint64_t h = fnv1a(s.data(), s.size());
    h = fnv1a(&"\x1f"[0], 1, h);
    h = fnv1a(p.data(), p.size(), h);
    h = fnv1a(&"\x1f"[0], 1, h);
    return fnv1a(o.data(), o.size(), h);
}

constexpr size_t TOKENIZE_MAX = 8192;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string jstr(const json_t* o, const char* k) {
    const json_t* v = json_object_get(o, k);
    return json_is_string(v) ? json_string_value(v) : "";
}

json_int_t jint(const json_t* o, const char* k) {
    const json_t* v = json_object_get(o, k);
    return json_is_integer(v) ? json_integer_value(v) : 0;
}

std::string build_meta(const std::string& kind, const std::string& lang, const std::string& rel,
                       json_int_t line, const std::string& name, const std::string& signature,
                       bool stub = false) {
    json_t* m = json_object();
    json_object_set_new(m, "kind", json_string(kind.c_str()));
    json_object_set_new(m, "lang", json_string(lang.c_str()));
    json_object_set_new(m, "file", json_string(rel.c_str()));
    json_object_set_new(m, "line", json_integer(line));
    json_object_set_new(m, "symbol", json_string(name.c_str()));
    if (!signature.empty()) json_object_set_new(m, "signature", json_string(signature.c_str()));
    if (stub) json_object_set_new(m, "stub", json_true());
    return dump_owned(m);
}

std::string build_content(const std::string& code, const std::string& docstring) {
    json_t* c = json_object();
    json_object_set_new(c, "code", json_string(code.c_str()));
    if (!docstring.empty()) json_object_set_new(c, "docstring", json_string(docstring.c_str()));
    return dump_owned(c);
}

std::string knowledge_row(const std::string& key, const std::string& meta, const std::string& content,
                          const std::string& terms) {
    return escape_copy(key) + "\t" + escape_copy(meta) + "\t" + escape_copy(content) + "\t" +
           escape_copy(terms) + "\n";
}

void ensure_predicates(Db& db) {
    const char* preds[] = {P_CALLS, P_DEFINES, P_ASSIGNS, P_USES};
    for (const char* p : preds) {
        db.exec(
            "INSERT INTO knowledge (key, meta, content, search_tsv) VALUES ($1, $2::jsonb, '{}'::jsonb, "
            "to_tsvector('simple', $3)) ON CONFLICT (key) DO NOTHING",
            {p, std::string(R"({"category":"predicate","name":")") + (p + 6) + R"("})", p + 6});
    }
}

// Streams a top-level JSON object ("key": value, ...) entry by entry, so large
// call_graph.json / dataflow.json never need to be loaded whole into memory.
class JsonObjectStream {
  public:
    explicit JsonObjectStream(const std::string& path) { f_ = std::fopen(path.c_str(), "rb"); }
    ~JsonObjectStream() {
        if (f_) std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }

    bool next(std::string& key, std::string& value) {
        if (!started_) {
            int c;
            do {
                c = get();
            } while (c >= 0 && std::isspace(c));
            if (c != '{') return false;
            started_ = true;
        }
        int c;
        do {
            c = get();
        } while (c == ',' || std::isspace(c));
        if (c < 0 || c == '}') return false;
        unget();

        std::string raw;
        if (!read_string_raw(raw)) return false;
        json_t* kj = json_loads(raw.c_str(), JSON_DECODE_ANY, nullptr);
        if (!json_is_string(kj)) {
            if (kj) json_decref(kj);
            return false;
        }
        key = json_string_value(kj);
        json_decref(kj);

        do {
            c = get();
        } while (std::isspace(c));
        if (c != ':') return false;
        return read_value_raw(value);
    }

  private:
    bool fill() {
        if (pos_ < buf_.size()) return true;
        buf_.resize(1 << 20);
        size_t n = std::fread(&buf_[0], 1, buf_.size(), f_);
        buf_.resize(n);
        pos_ = 0;
        return n > 0;
    }
    int get() { return fill() ? static_cast<unsigned char>(buf_[pos_++]) : -1; }
    void unget() {
        if (pos_ > 0) --pos_;
    }

    bool read_string_raw(std::string& out) {
        int c = get();
        if (c != '"') {
            unget();
            return false;
        }
        out.clear();
        out.push_back('"');
        while (true) {
            int d = get();
            if (d < 0) return false;
            out.push_back(static_cast<char>(d));
            if (d == '\\') {
                int e = get();
                if (e < 0) return false;
                out.push_back(static_cast<char>(e));
            } else if (d == '"') {
                return true;
            }
        }
    }

    bool read_value_raw(std::string& out) {
        int c;
        do {
            c = get();
        } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        if (c < 0) return false;
        out.clear();
        out.push_back(static_cast<char>(c));
        if (c == '{' || c == '[') {
            char open = static_cast<char>(c);
            char close = (c == '{') ? '}' : ']';
            int depth = 1;
            bool in_str = false, esc = false;
            while (depth > 0) {
                int d = get();
                if (d < 0) return false;
                out.push_back(static_cast<char>(d));
                if (in_str) {
                    if (esc) esc = false;
                    else if (d == '\\') esc = true;
                    else if (d == '"') in_str = false;
                } else {
                    if (d == '"') in_str = true;
                    else if (d == open) depth++;
                    else if (d == close) depth--;
                }
            }
            return true;
        }
        if (c == '"') {
            bool esc = false;
            while (true) {
                int d = get();
                if (d < 0) return false;
                out.push_back(static_cast<char>(d));
                if (esc) esc = false;
                else if (d == '\\') esc = true;
                else if (d == '"') return true;
            }
        }
        while (true) {
            int d = get();
            if (d < 0 || d == ',' || d == '}') {
                if (d >= 0) unget();
                break;
            }
            if (d == ' ' || d == '\n' || d == '\r' || d == '\t') {
                unget();
                break;
            }
            out.push_back(static_cast<char>(d));
        }
        return true;
    }

    FILE* f_{nullptr};
    std::string buf_;
    size_t pos_{0};
    bool started_{false};
};

}  // namespace

void import_analysis(Db& db, const ImportOptions& opt) {
    if (opt.analysis_dir.empty() || opt.project.empty() || opt.root.empty()) {
        throw std::runtime_error("[IMPORT] --analysis-dir, --project, --root are required");
    }

    std::unordered_map<std::string, std::vector<std::string>> name2keys;
    std::unordered_set<uint64_t> known_keys;
    std::unordered_map<std::string, std::pair<std::string, std::string>> stub_funcs;  // key -> (name, rel)
    std::unordered_map<std::string, std::string> stub_vars;                           // key -> var name

    double t0 = now_s();

    // Pass A: collect caller names so name2keys only stores names the call graph needs.
    std::unordered_set<std::string> caller_names;
    if (!opt.skip_callgraph) {
        JsonObjectStream cs(opt.analysis_dir + "/call_graph.json");
        if (cs.ok()) {
            std::string ck, cv;
            while (cs.next(ck, cv)) caller_names.insert(ck);
        }
        std::cerr << "[IMPORT] caller names: " << caller_names.size() << " (" << (now_s() - t0) << "s)\n";
    }

    db.exec("BEGIN");
    db.exec("CREATE TEMP TABLE stg_knowledge(key text, meta text, content text, terms text) ON COMMIT DROP");
    db.exec("CREATE TEMP TABLE stg_statement(subject text, predicate text, object text) ON COMMIT DROP");
    ensure_predicates(db);

    std::string kbuffer;
    long chunk_count = 0;
    auto flush_k = [&]() {
        if (kbuffer.empty()) return;
        db.copy_in("COPY stg_knowledge (key, meta, content, terms) FROM STDIN", kbuffer);
        kbuffer.clear();
    };
    auto add_knowledge = [&](const std::string& key, const std::string& meta, const std::string& content,
                             const std::string& terms) {
        kbuffer += knowledge_row(key, meta, content, terms);
        if (kbuffer.size() >= FLUSH_BYTES) flush_k();
    };

    {
        std::ifstream in(opt.analysis_dir + "/chunks_meta.jsonl");
        if (!in) throw std::runtime_error("[IMPORT] cannot open chunks_meta.jsonl");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (opt.limit > 0 && chunk_count >= opt.limit) break;
            json_error_t err;
            json_t* d = json_loads(line.c_str(), 0, &err);
            if (!d) continue;

            std::string name = jstr(d, "name");
            std::string file = jstr(d, "file");
            if (name.empty() || file.empty()) {
                json_decref(d);
                continue;
            }
            std::string rel = relpath_of(file, opt.root);
            std::string key = make_key(opt.project, rel, name);
            std::string meta = build_meta(jstr(d, "kind"), jstr(d, "language"), rel, jint(d, "line_start"),
                                          name, jstr(d, "signature"));
            std::string content = build_content(jstr(d, "content"), jstr(d, "docstring"));
            std::string terms = to_tsvector_text(tokenize_code(
                name + " " + jstr(d, "signature") + " " + truncate_utf8(jstr(d, "content"), TOKENIZE_MAX)));
            json_decref(d);

            if (known_keys.insert(hash64(key)).second) {
                add_knowledge(key, meta, content, terms);
                if (!opt.skip_callgraph && caller_names.count(name)) name2keys[name].push_back(key);
                chunk_count++;
            }
        }
        std::cerr << "[IMPORT] chunks: " << chunk_count << " entries (" << (now_s() - t0) << "s)\n";
    }

    long edges = 0;
    std::string ebuffer;
    std::unordered_set<uint64_t> seen_edges;
    auto flush_e = [&]() {
        if (ebuffer.empty()) return;
        db.copy_in("COPY stg_statement (subject, predicate, object) FROM STDIN", ebuffer);
        ebuffer.clear();
    };
    auto add_edge = [&](const std::string& s, const std::string& p, const std::string& o) {
        if (s.empty() || o.empty()) return;
        if (!seen_edges.insert(edge_hash(s, p, o)).second) return;
        ebuffer += escape_copy(s) + "\t" + escape_copy(p) + "\t" + escape_copy(o) + "\n";
        edges++;
        if (ebuffer.size() >= FLUSH_BYTES) flush_e();
    };
    auto note_endpoint = [&](const std::string& key, const std::string& name, const std::string& rel) {
        if (!known_keys.count(hash64(key))) stub_funcs.emplace(key, std::make_pair(name, rel));
    };

    if (!opt.skip_callgraph) {
        JsonObjectStream stream(opt.analysis_dir + "/call_graph.json");
        if (!stream.ok()) {
            std::cerr << "[IMPORT] call_graph.json not found, skipped\n";
        } else {
            std::string caller, value;
            while (stream.next(caller, value)) {
                auto it = name2keys.find(caller);
                if (it == name2keys.end()) continue;
                json_t* v = json_loads(value.c_str(), 0, nullptr);
                if (!v) continue;
                const json_t* calls = json_object_get(v, "calls");
                if (json_is_array(calls)) {
                    for (size_t i = 0; i < json_array_size(calls); ++i) {
                        const json_t* c = json_array_get(calls, i);
                        std::string callee = jstr(c, "function");
                        std::string cfile = jstr(c, "file");
                        if (callee.empty() || cfile.empty()) continue;
                        std::string rel = relpath_of(cfile, opt.root);
                        std::string okey = make_key(opt.project, rel, callee);
                        note_endpoint(okey, callee, rel);
                        // Ambiguous caller names (same name in many files) produce false
                        // edges; skip them unless fan-out is explicitly requested.
                        if (it->second.size() > 1 && !opt.fanout) continue;
                        for (const auto& skey : it->second) add_edge(skey, P_CALLS, okey);
                    }
                }
                json_decref(v);
            }
        }
    }

    if (!opt.skip_dataflow) {
        JsonObjectStream stream(opt.analysis_dir + "/dataflow.json");
        if (!stream.ok()) {
            std::cerr << "[IMPORT] dataflow.json not found, skipped\n";
        } else {
            std::string var, value;
            while (stream.next(var, value)) {
                json_t* v = json_loads(value.c_str(), 0, nullptr);
                if (!v) continue;
                std::string vkey = "/code/local/" + opt.project + "/vars/" + var;
                stub_vars.emplace(vkey, var);
                const json_t* occ = json_object_get(v, "occurrences");
                if (json_is_array(occ)) {
                    for (size_t i = 0; i < json_array_size(occ); ++i) {
                        const json_t* o = json_array_get(occ, i);
                        std::string ffile = jstr(o, "file");
                        std::string ffunc = jstr(o, "func");
                        std::string type = jstr(o, "type");
                        if (ffile.empty() || ffunc.empty()) continue;
                        std::string rel = relpath_of(ffile, opt.root);
                        std::string fkey = make_key(opt.project, rel, ffunc);
                        note_endpoint(fkey, ffunc, rel);
                        const char* pred = P_USES;
                        if (type == "definition") pred = P_DEFINES;
                        else if (type == "assignment") pred = P_ASSIGNS;
                        add_edge(fkey, pred, vkey);
                    }
                }
                json_decref(v);
            }
        }
    }

    for (const auto& [key, nf] : stub_funcs) {
        add_knowledge(key, build_meta("function", "", nf.second, 0, nf.first, "", true), "{}",
                      to_tsvector_text(tokenize_code(nf.first)));
    }
    for (const auto& [key, name] : stub_vars) {
        add_knowledge(key, build_meta("variable", "", "", 0, name, "", true), "{}",
                      to_tsvector_text(tokenize_code(name)));
    }
    flush_k();
    flush_e();

    std::cerr << "[IMPORT] stub funcs: " << stub_funcs.size() << ", vars: " << stub_vars.size()
              << ", edges: " << edges << " (" << (now_s() - t0) << "s)\n";

    double t_load = now_s();
    db.exec("SET LOCAL synchronous_commit = off");
    db.exec("SET LOCAL work_mem = '256MB'");
    db.exec("SET LOCAL maintenance_work_mem = '512MB'");

    // Drop secondary indexes / FKs only for large loads; for small projects the fixed
    // rebuild cost (~tens of seconds) outweighs per-row maintenance.
    const bool bulk = (chunk_count + edges) > 200000;

    // Referential integrity is guaranteed by the importer (stub endpoints exist for
    // every referenced key), so drop FKs during bulk load and rebuild them afterwards.
    if (bulk) {
        db.exec("ALTER TABLE statement DROP CONSTRAINT IF EXISTS statement_subject_id_fkey");
        db.exec("ALTER TABLE statement DROP CONSTRAINT IF EXISTS statement_predicate_id_fkey");
        db.exec("ALTER TABLE statement DROP CONSTRAINT IF EXISTS statement_object_id_fkey");
    }

    const char* drop_indexes[] = {
        "DROP INDEX IF EXISTS idx_knowledge_kind", "DROP INDEX IF EXISTS idx_knowledge_lang",
        "DROP INDEX IF EXISTS idx_knowledge_symbol", "DROP INDEX IF EXISTS idx_knowledge_file",
        "DROP INDEX IF EXISTS idx_knowledge_fti", "DROP INDEX IF EXISTS idx_knowledge_meta",
        "DROP INDEX IF EXISTS idx_knowledge_emb",
        "DROP INDEX IF EXISTS idx_stmt_fwd", "DROP INDEX IF EXISTS idx_stmt_rev",
        "DROP INDEX IF EXISTS idx_stmt_pred",
    };
    if (bulk) {
        for (const char* s : drop_indexes) db.exec(s);
    }

    double t_k = now_s();
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "SELECT key, meta::jsonb, content::jsonb, "
        "COALESCE(array_to_tsvector(string_to_array(NULLIF(terms, ''), ' ')), ''::tsvector) "
        "FROM stg_knowledge "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()");
    std::cerr << "[IMPORT] knowledge insert: " << (now_s() - t_k) << "s\n";

    double t_s = now_s();
    db.exec(
        "INSERT INTO statement (subject_id, predicate_id, object_id) "
        "SELECT s.id, p.id, o.id FROM stg_statement t "
        "JOIN knowledge s ON s.key = t.subject "
        "JOIN knowledge p ON p.key = t.predicate "
        "JOIN knowledge o ON o.key = t.object "
        "ON CONFLICT DO NOTHING");
    std::cerr << "[IMPORT] statement insert: " << (now_s() - t_s) << "s\n";

    if (bulk) {
        db.exec(
            "ALTER TABLE statement ADD CONSTRAINT statement_subject_id_fkey "
            "FOREIGN KEY (subject_id) REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED");
        db.exec(
            "ALTER TABLE statement ADD CONSTRAINT statement_predicate_id_fkey "
            "FOREIGN KEY (predicate_id) REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED");
        db.exec(
            "ALTER TABLE statement ADD CONSTRAINT statement_object_id_fkey "
            "FOREIGN KEY (object_id) REFERENCES knowledge(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED");
    }

    if (bulk) {
        double t_i = now_s();
        const char* create_indexes[] = {
            "CREATE INDEX idx_knowledge_kind ON knowledge ((meta->>'kind')) WHERE is_active",
            "CREATE INDEX idx_knowledge_lang ON knowledge ((meta->>'lang')) WHERE is_active",
            "CREATE INDEX idx_knowledge_symbol ON knowledge ((meta->>'symbol')) WHERE is_active",
            "CREATE INDEX idx_knowledge_file ON knowledge ((meta->>'file')) WHERE is_active",
            "CREATE INDEX idx_knowledge_fti ON knowledge USING GIN (search_tsv) WHERE is_active",
            "CREATE INDEX idx_knowledge_meta ON knowledge USING GIN (meta jsonb_path_ops) WHERE is_active",
            "CREATE INDEX idx_knowledge_emb ON knowledge USING hnsw (embedding vector_cosine_ops)",
            "CREATE INDEX idx_stmt_fwd ON statement (subject_id, predicate_id, object_id) WHERE is_active",
            "CREATE INDEX idx_stmt_rev ON statement (object_id, predicate_id, subject_id) WHERE is_active",
            "CREATE INDEX idx_stmt_pred ON statement (predicate_id) WHERE is_active",
        };
        for (const char* s : create_indexes) db.exec(s);
        std::cerr << "[IMPORT] index rebuild: " << (now_s() - t_i) << "s\n";
    }

    db.exec("DROP TABLE stg_knowledge");
    db.exec("DROP TABLE stg_statement");
    db.exec("COMMIT");
    std::cerr << "[IMPORT] db load: " << (now_s() - t_load) << "s, total: " << (now_s() - t0) << "s\n";
    std::cerr << "[IMPORT] done\n";
}
