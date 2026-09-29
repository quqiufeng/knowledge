#pragma once
#include <jansson.h>

#include <string>
#include <vector>

class Db {
public:
    explicit Db(const std::string& conninfo);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    json_t* query_json(const std::string& sql, const std::vector<std::string>& params);
    void exec(const std::string& sql, const std::vector<std::string>& params = {});
    void copy_in(const std::string& copy_sql, const std::string& data);

private:
    struct pg_conn* conn_;
};

std::string default_conninfo();
