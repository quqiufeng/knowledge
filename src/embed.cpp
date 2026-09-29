#include "embed.hpp"

#include "util.hpp"

#include <jansson.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t FLUSH_ROWS = 2000;

std::string make_key(const std::string& project, const std::string& rel, const std::string& name) {
    return "/code/local/" + project + "/" + rel + "/" + name;
}

std::string jstr(const json_t* o, const char* k) {
    const json_t* v = json_object_get(o, k);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string vec_literal(const float* v, int dim) {
    std::string out = "[";
    char buf[32];
    for (int i = 0; i < dim; ++i) {
        std::snprintf(buf, sizeof(buf), "%.7g", v[i]);
        if (i) out += ",";
        out += buf;
    }
    out += "]";
    return out;
}

}  // namespace

long import_vectors(Db& db, const EmbedOptions& opt) {
    if (opt.bin_file.empty() || opt.meta_file.empty() || opt.project.empty() || opt.root.empty())
        throw std::runtime_error("[EMBED] bin-file/meta-file/project/root are required");

    std::ifstream bin(opt.bin_file, std::ios::binary);
    if (!bin) throw std::runtime_error("[EMBED] cannot open " + opt.bin_file);
    std::ifstream meta(opt.meta_file);
    if (!meta) throw std::runtime_error("[EMBED] cannot open " + opt.meta_file);

    uint32_t count = 0, dim = 0;
    bin.read(reinterpret_cast<char*>(&count), 4);
    bin.read(reinterpret_cast<char*>(&dim), 4);
    if (!bin || dim == 0) throw std::runtime_error("[EMBED] bad bin header");
    if (dim != 768) throw std::runtime_error("[EMBED] expected 768 dims, got " + std::to_string(dim));

    db.exec("BEGIN");
    db.exec("CREATE TEMP TABLE stg_vec(key text, vec text) ON COMMIT DROP");

    std::string buf;
    long rows = 0, n = 0;
    auto flush = [&]() {
        if (buf.empty()) return;
        db.copy_in("COPY stg_vec (key, vec) FROM STDIN", buf);
        buf.clear();
    };

    std::vector<float> vec(dim);
    std::string line;
    while (std::getline(meta, line)) {
        // read one bin record: uint32 name_len, name, dim floats
        uint32_t name_len = 0;
        if (!bin.read(reinterpret_cast<char*>(&name_len), 4)) break;
        if (name_len > (1u << 20)) break;
        std::string name(name_len, '\0');
        if (!bin.read(&name[0], name_len)) break;
        if (!bin.read(reinterpret_cast<char*>(vec.data()), dim * sizeof(float))) break;

        if (line.empty()) continue;
        json_t* d = json_loads(line.c_str(), 0, nullptr);
        if (!json_is_object(d)) {
            if (d) json_decref(d);
            continue;
        }
        std::string fname = jstr(d, "name");
        std::string file = jstr(d, "file");
        json_decref(d);
        if (fname.empty() || file.empty()) continue;

        std::string key = make_key(opt.project, kutil::relpath_of(file, opt.root), fname);
        buf += kutil::escape_copy(key) + "\t" + kutil::escape_copy(vec_literal(vec.data(), dim)) + "\n";
        rows++;
        if (++n % FLUSH_ROWS == 0) flush();
        if (opt.limit > 0 && rows >= opt.limit) break;
    }
    flush();

    // HNSW is expensive to maintain per row; for large loads drop and rebuild it.
    bool bulk = rows > 20000;
    if (bulk) db.exec("DROP INDEX IF EXISTS idx_knowledge_emb");
    db.exec(
        "UPDATE knowledge k SET embedding = v.vec::vector "
        "FROM (SELECT DISTINCT ON (key) key, vec FROM stg_vec) v WHERE k.key = v.key");
    if (bulk)
        db.exec("CREATE INDEX idx_knowledge_emb ON knowledge USING hnsw (embedding vector_cosine_ops)");
    db.exec("DROP TABLE stg_vec");
    db.exec("COMMIT");
    return rows;
}
