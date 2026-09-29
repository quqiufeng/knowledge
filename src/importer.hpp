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
};

void import_analysis(Db& db, const ImportOptions& opt);
