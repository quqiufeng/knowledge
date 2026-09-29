#pragma once
#include "db.hpp"

#include <string>

struct ExportOptions {
    std::string preset;       // "edges" | "context"
    std::string predicate;    // edges: filter by predicate key (e.g. /pred/calls)
    std::string key_prefix;   // filter subject/entry key prefix
    long limit{1000};
    int depth{1};             // context: relation depth
    bool no_content{false};   // context: omit source text
};

// Write JSONL training records to stdout.
long export_corpus(Db& db, const ExportOptions& opt);
