#include "books.hpp"

#include "util.hpp"

#include <jansson.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace kutil;

constexpr size_t FLUSH_BYTES = 8 * 1024 * 1024;
const char* P_CONTAINS = "/pred/contains";

std::string clean_page(std::string md) {
    auto trim_front = [](std::string& s) {
        while (!s.empty() && (s[0] == '\n' || s[0] == '\r' || s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    };
    trim_front(md);
    if (md.rfind("---", 0) == 0) {
        size_t end = md.find("\n---", 3);
        if (end != std::string::npos) {
            size_t nl = md.find('\n', end + 1);
            md = (nl == std::string::npos) ? "" : md.substr(nl + 1);
        }
    }
    trim_front(md);
    if (md.rfind("<!--", 0) == 0) {
        size_t end = md.find("-->");
        if (end != std::string::npos) md = md.substr(end + 3);
    }
    trim_front(md);
    return md;
}

int chapter_order(const std::string& name) {
    int n = 0;
    size_t i = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
        n = n * 10 + (name[i] - '0');
        ++i;
    }
    return n;
}

class Stage {
  public:
    explicit Stage(Db& db) : db_(db) {
        db_.exec("CREATE TEMP TABLE stg_knowledge(key text, meta text, content text, terms text) ON COMMIT DROP");
        db_.exec("CREATE TEMP TABLE stg_statement(subject text, predicate text, object text) ON COMMIT DROP");
    }
    void add_knowledge(const std::string& key, const std::string& meta, const std::string& content,
                       const std::string& terms) {
        kbuf_ += escape_copy(key) + "\t" + escape_copy(meta) + "\t" + escape_copy(content) + "\t" +
                 escape_copy(terms) + "\n";
        if (kbuf_.size() >= FLUSH_BYTES) flush_k();
    }
    void add_edge(const std::string& s, const std::string& p, const std::string& o) {
        ebuf_ += escape_copy(s) + "\t" + escape_copy(p) + "\t" + escape_copy(o) + "\n";
        edges_++;
        if (ebuf_.size() >= FLUSH_BYTES) flush_e();
    }
    void finish() {
        flush_k();
        flush_e();
        db_.exec(
            "INSERT INTO knowledge (key, meta, content, search_tsv) "
            "SELECT key, meta::jsonb, content::jsonb, to_tsvector('simple', terms) FROM stg_knowledge "
            "ON CONFLICT (key) DO UPDATE SET meta = EXCLUDED.meta, content = EXCLUDED.content, "
            "search_tsv = EXCLUDED.search_tsv, version = knowledge.version + 1, updated_at = now()");
        db_.exec(
            "INSERT INTO statement (subject_id, predicate_id, object_id) "
            "SELECT s.id, p.id, o.id FROM stg_statement t "
            "JOIN knowledge s ON s.key = t.subject JOIN knowledge p ON p.key = t.predicate "
            "JOIN knowledge o ON o.key = t.object ON CONFLICT DO NOTHING");
    }
    long edges() const { return edges_; }

  private:
    void flush_k() {
        if (kbuf_.empty()) return;
        db_.copy_in("COPY stg_knowledge (key, meta, content, terms) FROM STDIN", kbuf_);
        kbuf_.clear();
    }
    void flush_e() {
        if (ebuf_.empty()) return;
        db_.copy_in("COPY stg_statement (subject, predicate, object) FROM STDIN", ebuf_);
        ebuf_.clear();
    }
    Db& db_;
    std::string kbuf_, ebuf_;
    long edges_{0};
};

}  // namespace

void import_books(Db& db, const BooksOptions& opt) {
    if (!fs::is_directory(opt.books_dir)) {
        throw std::runtime_error("[BOOKS] books dir not found: " + opt.books_dir);
    }

    db.exec("BEGIN");
    db.exec(
        "INSERT INTO knowledge (key, meta, content, search_tsv) VALUES ($1, $2::jsonb, '{}'::jsonb, "
        "to_tsvector('simple', 'contains')) ON CONFLICT (key) DO NOTHING",
        {P_CONTAINS, R"({"category":"predicate","name":"contains"})"});

    Stage stage(db);
    long pages = 0, books = 0;

    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(opt.books_dir)) {
        if (!e.is_directory()) continue;
        std::string name = e.path().filename().string();
        if (!opt.only_book.empty() && name != opt.only_book) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());

    for (const auto& book : names) {
        fs::path bdir = fs::path(opt.books_dir) / book;
        if (!fs::is_directory(bdir / "chapters")) continue;

        std::string title = book, author;
        std::string meta_raw = read_file((bdir / "_meta.json").string());
        if (!meta_raw.empty()) {
            json_t* m = json_loads(meta_raw.c_str(), 0, nullptr);
            if (json_is_object(m)) {
                const json_t* t = json_object_get(m, "title");
                const json_t* a = json_object_get(m, "author");
                if (json_is_string(t)) title = json_string_value(t);
                if (json_is_string(a)) author = json_string_value(a);
            }
            if (m) json_decref(m);
        }

        std::string book_key = "/books/" + book;
        json_t* bmeta = json_object();
        json_object_set_new(bmeta, "kind", json_string("book"));
        json_object_set_new(bmeta, "symbol", json_string(book.c_str()));
        json_object_set_new(bmeta, "title", json_string(title.c_str()));
        if (!author.empty()) json_object_set_new(bmeta, "author", json_string(author.c_str()));
        stage.add_knowledge(book_key, dump_owned(bmeta), "{}", title + " " + author);
        books++;

        std::vector<fs::path> chapters;
        for (const auto& c : fs::directory_iterator(bdir / "chapters")) {
            if (c.is_directory()) chapters.push_back(c.path());
        }
        std::sort(chapters.begin(), chapters.end());

        for (const auto& cpath : chapters) {
            std::string chapter = cpath.filename().string();
            std::string chapter_key = book_key + "/chapters/" + chapter;
            json_t* cmeta = json_object();
            json_object_set_new(cmeta, "kind", json_string("book_chapter"));
            json_object_set_new(cmeta, "book", json_string(book.c_str()));
            json_object_set_new(cmeta, "chapter", json_string(chapter.c_str()));
            json_object_set_new(cmeta, "order", json_integer(chapter_order(chapter)));
            stage.add_knowledge(chapter_key, dump_owned(cmeta), "{}", chapter);
            stage.add_edge(book_key, P_CONTAINS, chapter_key);

            std::vector<fs::path> files;
            for (const auto& f : fs::directory_iterator(cpath)) {
                if (f.is_regular_file() && f.path().extension() == ".md") files.push_back(f.path());
            }
            std::sort(files.begin(), files.end());

            for (const auto& fpath : files) {
                if (opt.limit > 0 && pages >= opt.limit) break;
                std::string page = fpath.stem().string();
                std::string rel = "chapters/" + chapter + "/" + fpath.filename().string();
                std::string page_key = book_key + "/chapters/" + chapter + "/" + page;
                std::string text = clean_page(read_file(fpath.string()));

                json_t* pmeta = json_object();
                json_object_set_new(pmeta, "kind", json_string("book_page"));
                json_object_set_new(pmeta, "book", json_string(book.c_str()));
                json_object_set_new(pmeta, "chapter", json_string(chapter.c_str()));
                json_object_set_new(pmeta, "page", json_string(page.c_str()));
                json_object_set_new(pmeta, "file", json_string(rel.c_str()));

                json_t* pcontent = json_object();
                json_object_set_new(pcontent, "text", json_string(text.c_str()));

                stage.add_knowledge(page_key, dump_owned(pmeta), dump_owned(pcontent),
                                    truncate_utf8(text, 4096));
                stage.add_edge(chapter_key, P_CONTAINS, page_key);
                pages++;
            }
        }
    }

    stage.finish();
    db.exec("DROP TABLE stg_knowledge");
    db.exec("DROP TABLE stg_statement");
    db.exec("COMMIT");
    std::cerr << "[BOOKS] books: " << books << ", pages: " << pages << ", edges: " << stage.edges() << "\n";
}
