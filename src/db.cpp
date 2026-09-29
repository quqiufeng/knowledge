#include "db.hpp"

#include <libpq-fe.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

std::string default_conninfo() {
    const char* env = std::getenv("DATABASE_URL");
    if (env && *env) return env;
    return "postgresql://knowledge:knowledge@127.0.0.1:5432/knowledge";
}

Db::Db(const std::string& conninfo) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_);
        PQfinish(conn_);
        throw std::runtime_error("[DB] connection failed: " + err);
    }
}

Db::~Db() {
    if (conn_) PQfinish(conn_);
}

static std::vector<const char*> to_values(const std::vector<std::string>& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) values.push_back(p.c_str());
    return values;
}

json_t* Db::query_json(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<const char*> values = to_values(params);
    PGresult* res = PQexecParams(conn_, sql.c_str(), static_cast<int>(params.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!res) throw std::runtime_error("[DB] PQexecParams returned null");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("[DB] query failed: " + err);
    }

    json_t* arr = json_array();
    int rows = PQntuples(res);
    int cols = PQnfields(res);
    for (int r = 0; r < rows; ++r) {
        json_t* obj = json_object();
        for (int c = 0; c < cols; ++c) {
            const char* name = PQfname(res, c);
            if (PQgetisnull(res, r, c)) {
                json_object_set_new(obj, name, json_null());
            } else {
                const char* val = PQgetvalue(res, r, c);
                if (std::strcmp(name, "meta") == 0 || std::strcmp(name, "content") == 0) {
                    json_t* nested = json_loads(val, 0, nullptr);
                    if (nested) {
                        json_object_set_new(obj, name, nested);
                        continue;
                    }
                }
                json_object_set_new(obj, name, json_string(val));
            }
        }
        json_array_append_new(arr, obj);
    }
    PQclear(res);
    return arr;
}

void Db::copy_in(const std::string& copy_sql, const std::string& data) {
    PGresult* res = PQexec(conn_, copy_sql.c_str());
    if (!res || PQresultStatus(res) != PGRES_COPY_IN) {
        std::string err = res ? PQresultErrorMessage(res) : "no result";
        if (res) PQclear(res);
        throw std::runtime_error("[DB] COPY init failed: " + err);
    }
    PQclear(res);

    if (!data.empty()) {
        if (PQputCopyData(conn_, data.data(), static_cast<int>(data.size())) != 1) {
            throw std::runtime_error(std::string("[DB] PQputCopyData failed: ") + PQerrorMessage(conn_));
        }
    }
    if (PQputCopyEnd(conn_, nullptr) != 1) {
        throw std::runtime_error(std::string("[DB] PQputCopyEnd failed: ") + PQerrorMessage(conn_));
    }
    res = PQgetResult(conn_);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = res ? PQresultErrorMessage(res) : "no result";
        if (res) PQclear(res);
        throw std::runtime_error("[DB] COPY failed: " + err);
    }
    PQclear(res);
}

void Db::exec(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<const char*> values = to_values(params);
    PGresult* res = PQexecParams(conn_, sql.c_str(), static_cast<int>(params.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!res) throw std::runtime_error("[DB] PQexecParams returned null");
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("[DB] exec failed: " + err);
    }
    PQclear(res);
}
