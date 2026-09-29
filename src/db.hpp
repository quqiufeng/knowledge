#pragma once
#include <jansson.h>

#include <string>
#include <vector>

class Db {
public:
    explicit Db(const std::string& conninfo, int statement_timeout_ms = 0);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    json_t* query_json(const std::string& sql, const std::vector<std::string>& params);
    void exec(const std::string& sql, const std::vector<std::string>& params = {});
    void copy_in(const std::string& copy_sql, const std::string& data);
    bool healthy() const;

    // RAII transaction: ROLLBACK on destruction unless commit() was called.
    class Tx {
    public:
        explicit Tx(Db& db) : db_(db), active_(true) { db_.exec("BEGIN"); }
        ~Tx() {
            if (active_) {
                try {
                    db_.exec("ROLLBACK");
                } catch (...) {
                }
            }
        }
        Tx(const Tx&) = delete;
        Tx& operator=(const Tx&) = delete;
        void commit() {
            db_.exec("COMMIT");
            active_ = false;
        }

    private:
        Db& db_;
        bool active_;
    };

private:
    struct pg_conn* conn_;
};

std::string default_conninfo();
