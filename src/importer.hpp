#pragma once
#include "db.hpp"

#include <string>

struct ImportOptions {
    std::string analysis_dir;
    std::string project;
    std::string root;
    long limit{0};
    bool skip_callgraph{false};
    bool skip_dataflow{false};
    bool fanout{false};  // resolve callers by name even when the name is ambiguous
};

void import_analysis(Db& db, const ImportOptions& opt);
