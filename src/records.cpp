#include "records.hpp"

#include "util.hpp"

#include <jansson.h>

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t FLUSH_BYTES = 8 * 1024 * 1024;

std::string basename_of(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string key_of(const json_t* rec, const std::string& field) {
    const json_t* v = json_object_get(rec, field.c_str());
    if (json_is_string(v)) return json_string_value(v);
    if (json_is_integer(v)) return std::to_string(json_integer_value(v));
    if (json_is_real(v)) return std::to_string(json_real_value(v));
    return "";
}

// Minimal CSV parser: one line -> fields, handling double-quoted fields with "".
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

class Stage {
  public:
    explicit Stage(Db& db) : db_(db) {
        db_.exec("CREATE TEMP TABLE stg_knowledge(key text, meta text, content text, terms text) ON COMMIT DROP");
    }
    void add(const std::string& key, const std::string& meta, const std::string& content,
             const std::string& terms) {
        buf_ += kutil::escape_copy(key) + "\t" + kutil::escape_copy(meta) + "\t" +
                kutil::escape_copy(content) + "\t" + kutil::escape_copy(terms) + "\n";
        count_++;
        if (buf_.size() >= FLUSH_BYTES) flush();
    }
    void finish() {
        flush();
        db_.exec(
            "INSERT INTO knowledge (key, meta, content, search_tsv) "
            "SELECT key, meta::jsonb, content::jsonb, to_tsvector('simple', terms) FROM stg_knowledge "
            "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
            "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()");
    }
    long count() const { return count_; }

  private:
    void flush() {
        if (buf_.empty()) return;
        db_.copy_in("COPY stg_knowledge (key, meta, content, terms) FROM STDIN", buf_);
        buf_.clear();
    }
    Db& db_;
    std::string buf_;
    long count_{0};
};

void emit(Stage& stage, const RecordsOptions& opt, const std::string& source, const json_t* rec) {
    std::string kv = key_of(rec, opt.key_field);
    if (kv.empty()) return;
    std::string key = opt.prefix + kv;

    json_t* meta = json_object();
    json_object_set_new(meta, "kind", json_string(opt.kind.c_str()));
    json_object_set_new(meta, "source", json_string(source.c_str()));
    const char* k;
    json_t* v;
    json_object_foreach(const_cast<json_t*>(rec), k, v) {
        if (k == opt.key_field || (!opt.text_field.empty() && k == opt.text_field)) continue;
        if (json_is_string(v) || json_is_number(v) || json_is_boolean(v)) {
            json_object_set(meta, k, v);
        }
    }

    json_t* content = json_object();
    std::string terms;
    if (!opt.text_field.empty()) {
        std::string text = key_of(rec, opt.text_field);
        json_object_set_new(content, "text", json_string(text.c_str()));
        terms = text;
    } else {
        json_object_set_new(content, "value", json_deep_copy(rec));
        terms = kutil::dump_owned(json_deep_copy(rec));
    }

    stage.add(key, kutil::dump_owned(meta), kutil::dump_owned(content), terms);
}

}  // namespace

long import_records(Db& db, const RecordsOptions& opt) {
    if (opt.file.empty()) throw std::runtime_error("[RECORDS] file is required");
    std::string format = opt.format;
    if (format.empty()) {
        auto ends = [&](const char* s) {
            std::string e = s;
            return opt.file.size() >= e.size() && opt.file.compare(opt.file.size() - e.size(), e.size(), e) == 0;
        };
        format = ends(".csv") ? "csv" : "jsonl";
    }
    if (format != "jsonl" && format != "csv")
        throw std::runtime_error("[RECORDS] format must be jsonl or csv");

    std::ifstream in(opt.file);
    if (!in) throw std::runtime_error("[RECORDS] cannot open " + opt.file);
    std::string source = basename_of(opt.file);

    db.exec("BEGIN");
    Stage stage(db);

    if (format == "jsonl") {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json_t* rec = json_loads(line.c_str(), 0, nullptr);
            if (!json_is_object(rec)) {
                if (rec) json_decref(rec);
                continue;
            }
            emit(stage, opt, source, rec);
            json_decref(rec);
        }
    } else {
        auto strip_cr = [](std::string& s) {
            if (!s.empty() && s.back() == '\r') s.pop_back();
        };
        std::string header;
        if (!std::getline(in, header)) throw std::runtime_error("[RECORDS] empty csv");
        strip_cr(header);
        std::vector<std::string> cols = parse_csv_line(header);
        std::string line;
        while (std::getline(in, line)) {
            strip_cr(line);
            if (line.empty()) continue;
            std::vector<std::string> vals = parse_csv_line(line);
            json_t* rec = json_object();
            for (size_t i = 0; i < cols.size() && i < vals.size(); ++i) {
                json_object_set_new(rec, cols[i].c_str(), json_string(vals[i].c_str()));
            }
            emit(stage, opt, source, rec);
            json_decref(rec);
        }
    }

    stage.finish();
    db.exec("DROP TABLE stg_knowledge");
    db.exec("COMMIT");
    return stage.count();
}
