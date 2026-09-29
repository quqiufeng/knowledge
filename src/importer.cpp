#include "importer.hpp"

#include "tokenize.hpp"
#include "util.hpp"

#include <jansson.h>

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

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path);
    if (!in) {
        ok = false;
        return "";
    }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

void import_analysis(Db& db, const ImportOptions& opt) {
    if (opt.analysis_dir.empty() || opt.project.empty() || opt.root.empty()) {
        throw std::runtime_error("[IMPORT] --analysis-dir, --project, --root are required");
    }

    std::unordered_map<std::string, std::vector<std::string>> name2keys;
    std::unordered_set<std::string> known_keys;
    std::unordered_map<std::string, std::pair<std::string, std::string>> stub_funcs;  // key -> (name, rel)
    std::unordered_map<std::string, std::string> stub_vars;                           // key -> var name

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
            std::string terms = to_tsvector_text(
                tokenize_code(name + " " + jstr(d, "signature") + " " + jstr(d, "content")));
            json_decref(d);

            if (known_keys.insert(key).second) {
                add_knowledge(key, meta, content, terms);
                name2keys[name].push_back(key);
                chunk_count++;
            }
        }
        std::cerr << "[IMPORT] chunks: " << chunk_count << " entries\n";
    }

    long edges = 0;
    std::string ebuffer;
    std::unordered_set<std::string> seen_edges;
    auto flush_e = [&]() {
        if (ebuffer.empty()) return;
        db.copy_in("COPY stg_statement (subject, predicate, object) FROM STDIN", ebuffer);
        ebuffer.clear();
    };
    auto add_edge = [&](const std::string& s, const std::string& p, const std::string& o) {
        if (s.empty() || o.empty()) return;
        if (!seen_edges.insert(s + "\x1f" + p + "\x1f" + o).second) return;
        ebuffer += escape_copy(s) + "\t" + escape_copy(p) + "\t" + escape_copy(o) + "\n";
        edges++;
        if (ebuffer.size() >= FLUSH_BYTES) flush_e();
    };
    auto note_endpoint = [&](const std::string& key, const std::string& name, const std::string& rel) {
        if (!known_keys.count(key)) stub_funcs.emplace(key, std::make_pair(name, rel));
    };

    if (!opt.skip_callgraph) {
        bool ok;
        std::string content = read_file(opt.analysis_dir + "/call_graph.json", ok);
        json_t* cg = ok ? json_loads(content.c_str(), 0, nullptr) : nullptr;
        if (!cg) {
            std::cerr << "[IMPORT] call_graph.json missing or invalid, skipped\n";
        } else if (json_is_object(cg)) {
            const char* caller;
            json_t* v;
            json_object_foreach(cg, caller, v) {
                auto it = name2keys.find(caller);
                if (it == name2keys.end()) continue;
                const json_t* calls = json_object_get(v, "calls");
                if (!json_is_array(calls)) continue;
                for (size_t i = 0; i < json_array_size(calls); ++i) {
                    const json_t* c = json_array_get(calls, i);
                    std::string callee = jstr(c, "function");
                    std::string cfile = jstr(c, "file");
                    if (callee.empty() || cfile.empty()) continue;
                    std::string rel = relpath_of(cfile, opt.root);
                    std::string okey = make_key(opt.project, rel, callee);
                    note_endpoint(okey, callee, rel);
                    for (const auto& skey : it->second) add_edge(skey, P_CALLS, okey);
                }
            }
            json_decref(cg);
        }
    }

    if (!opt.skip_dataflow) {
        bool ok;
        std::string content = read_file(opt.analysis_dir + "/dataflow.json", ok);
        json_t* df = ok ? json_loads(content.c_str(), 0, nullptr) : nullptr;
        if (!df) {
            std::cerr << "[IMPORT] dataflow.json missing or invalid, skipped\n";
        } else if (json_is_object(df)) {
            const char* var;
            json_t* v;
            json_object_foreach(df, var, v) {
                std::string vkey = "/code/local/" + opt.project + "/vars/" + var;
                stub_vars.emplace(vkey, var);
                const json_t* occ = json_object_get(v, "occurrences");
                if (!json_is_array(occ)) continue;
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
            json_decref(df);
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
              << ", edges: " << edges << "\n";

    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) "
        "SELECT key, meta::jsonb, content::jsonb, to_tsvector('simple', terms) FROM stg_knowledge "
        "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
        "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()");
    db.exec(
        "INSERT INTO statement (subject_id, predicate_id, object_id) "
        "SELECT s.id, p.id, o.id FROM stg_statement t "
        "JOIN knowledge s ON s.key = t.subject "
        "JOIN knowledge p ON p.key = t.predicate "
        "JOIN knowledge o ON o.key = t.object "
        "ON CONFLICT DO NOTHING");
    db.exec("DROP TABLE stg_knowledge");
    db.exec("DROP TABLE stg_statement");
    db.exec("COMMIT");
    std::cerr << "[IMPORT] done\n";
}
