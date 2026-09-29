#pragma once
#include "db.hpp"

#include <string>

struct EmbedOptions {
    std::string bin_file;   // path to *.jina.bin
    std::string meta_file;  // path to chunks_meta.jsonl
    std::string project;
    std::string root;
    long limit{0};
};

// Load vectors from a my_db *.jina.bin into knowledge.embedding, aligned by
// chunks_meta.jsonl order (record i <-> meta line i).
long import_vectors(Db& db, const EmbedOptions& opt);
