#pragma once
#include "db.hpp"

#include <string>

struct RecordsOptions {
    std::string file;
    std::string format;              // "jsonl" | "csv" (auto by extension if empty)
    std::string key_field{"key"};    // field holding the entry key
    std::string kind{"record"};      // meta.kind
    std::string text_field;          // optional: field used as content.text + full-text
    std::string prefix;              // optional key prefix
};

// Generic importer: any JSONL/CSV -> knowledge entries.
long import_records(Db& db, const RecordsOptions& opt);
