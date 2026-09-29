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
    std::string category;            // optional: validate against /spec/{category}
};

struct RecordsResult {
    long imported{0};  // rows staged
    long skipped{0};   // invalid json / missing key (reported, never silent)
};

// Generic importer: any JSONL/CSV -> knowledge entries.
RecordsResult import_records(Db& db, const RecordsOptions& opt);
