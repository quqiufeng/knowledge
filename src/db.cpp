#include "db.hpp"

#include <libpq-fe.h>

#include <cerrno>
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

Db::Db(const std::string& conninfo, int statement_timeout_ms) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_);
        PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("[DB] connection failed: " + err);
    }
    if (statement_timeout_ms > 0) {
        exec("SET statement_timeout = " + std::to_string(statement_timeout_ms));
    }
}

bool Db::healthy() const {
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
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

// Convert a text-formatted PG value to JSON using the column type (OID).
// Only types with an unambiguous JSON representation are converted; everything
// else stays a string. This keeps counts/ids as numbers and jsonb as objects.
static json_t* value_to_json(Oid type, const char* val) {
    switch (type) {
        case 16:  // bool
            return json_boolean(val[0] == 't');
        case 20:  // int8
        case 21:  // int2
        case 23: {  // int4
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(val, &end, 10);
            if (!errno && end && *end == '\0') return json_integer(v);
            break;
        }
        case 700:  // float4
        case 701:  // float8
        case 1700: {  // numeric
            errno = 0;
            char* end = nullptr;
            double d = std::strtod(val, &end);
            if (!errno && end && *end == '\0') {
                // Counts and similar whole numbers stay integers.
                if (d == static_cast<double>(static_cast<long long>(d)) &&
                    d >= -9007199254740992.0 && d <= 9007199254740992.0) {
                    return json_integer(static_cast<long long>(d));
                }
                return json_real(d);
            }
            break;
        }
        case 114:   // json
        case 3802: {  // jsonb
            // JSON_DECODE_ANY: jsonb scalars (312, "x", true, null) are legal too.
            json_t* nested = json_loads(val, JSON_DECODE_ANY, nullptr);
            if (nested) return nested;
            break;
        }
        default:
            break;
    }
    return json_string(val);
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
                json_object_set_new(obj, name, value_to_json(PQftype(res, c), val));
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
