#pragma once
#include <string>

struct ServerOptions {
    int port{8931};
    std::string bind{"127.0.0.1"};
    int pool{4};
    bool read_only{false};
    std::string conninfo;
    std::string vector_cmd;
};

int run_server(const ServerOptions& opt);
