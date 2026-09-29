#include "server.hpp"

#include "api.hpp"
#include "db.hpp"

#include "httplib.h"

#include <jansson.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string dump(const json_t* v, size_t flags = JSON_COMPACT) {
    char* s = json_dumps(v, flags);
    std::string out = s ? s : "";
    if (s) free(s);
    return out;
}

class Pool {
  public:
    Pool(const std::string& conninfo, int n) {
        for (int i = 0; i < n; ++i) {
            auto db = std::make_unique<Db>(conninfo);
            db->exec("SET statement_timeout = '30s'");
            conns_.push_back(std::move(db));
        }
    }

    class Lease {
      public:
        Lease(Pool& p) : pool_(p), db_(p.acquire()) {}
        ~Lease() { pool_.release(db_); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Db& db() const { return *db_; }

      private:
        Pool& pool_;
        Db* db_;
    };

  private:
    Db* acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !conns_.empty(); });
        Db* db = conns_.back().release();
        conns_.pop_back();
        return db;
    }
    void release(Db* db) {
        {
            std::lock_guard<std::mutex> lk(m_);
            conns_.push_back(std::unique_ptr<Db>(db));
        }
        cv_.notify_one();
    }

    std::vector<std::unique_ptr<Db>> conns_;
    std::mutex m_;
    std::condition_variable cv_;
};

void set_json(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}

void fail(httplib::Response& res, int status, const std::string& msg) {
    json_t* e = json_object();
    json_object_set_new(e, "error", json_string(msg.c_str()));
    set_json(res, status, dump(e));
    json_decref(e);
}

std::vector<VectorHit> vectors_from(const json_t* arr) {
    std::vector<VectorHit> out;
    if (!json_is_array(arr)) return out;
    for (size_t i = 0; i < json_array_size(arr); ++i) {
        const json_t* e = json_array_get(arr, i);
        const json_t* k = json_object_get(e, "key");
        if (!json_is_string(k)) continue;
        VectorHit h;
        h.key = json_string_value(k);
        const json_t* s = json_object_get(e, "score");
        if (json_is_number(s)) h.score = json_number_value(s);
        out.push_back(h);
    }
    return out;
}

}  // namespace

int run_server(const ServerOptions& opt) {
    std::string conninfo = opt.conninfo;
    if (conninfo.empty()) conninfo = default_conninfo();
    Pool pool(conninfo, opt.pool > 0 ? opt.pool : 4);

    httplib::Server svr;
    svr.set_payload_max_length(1 << 20);

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        set_json(res, 200, R"({"status":"ok"})");
    });

    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        Pool::Lease lease(pool);
        json_t* rows = lease.db().query_json(
            "SELECT (SELECT count(*) FROM knowledge) AS knowledge, "
            "(SELECT count(*) FROM statement) AS statement, "
            "(SELECT count(*) FROM knowledge WHERE is_active) AS active",
            {});
        set_json(res, 200, dump(json_array_get(rows, 0), JSON_INDENT(2)));
        json_decref(rows);
    });

    // POST /jse  body: JSE query object; optional "$vectors": [{key,score}] for $search
    svr.Post("/jse", [&](const httplib::Request& req, httplib::Response& res) {
        json_error_t err;
        json_t* body = json_loads(req.body.c_str(), 0, &err);
        if (!body) return fail(res, 400, std::string("invalid JSON: ") + err.text);
        std::vector<VectorHit> vectors = vectors_from(json_object_get(body, "$vectors"));
        try {
            Pool::Lease lease(pool);
            std::unordered_map<std::string, long long> cache;
            CompiledQuery cq = compile_query(body, make_options(lease.db(), vectors, cache));
            json_t* rows = lease.db().query_json(cq.sql, cq.params);
            set_json(res, 200, dump(rows, JSON_INDENT(2)));
            json_decref(rows);
        } catch (const std::exception& e) {
            json_decref(body);
            return fail(res, 400, e.what());
        }
        json_decref(body);
    });

    svr.Get("/search", [&](const httplib::Request& req, httplib::Response& res) {
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        if (q.empty()) return fail(res, 400, "missing ?q=");
        int k = req.has_param("k") ? std::atoi(req.get_param_value("k").c_str()) : 10;
        std::string where = req.has_param("where") ? req.get_param_value("where") : "";
        try {
            Pool::Lease lease(pool);
            json_t* r = api_search(lease.db(), q, k, opt.vector_cmd, where);
            set_json(res, 200, dump(r, JSON_INDENT(2)));
            json_decref(r);
        } catch (const std::exception& e) {
            fail(res, 400, e.what());
        }
    });

    svr.Get("/context", [&](const httplib::Request& req, httplib::Response& res) {
        std::string key = req.has_param("key") ? req.get_param_value("key") : "";
        if (key.empty()) return fail(res, 400, "missing ?key=");
        int depth = req.has_param("depth") ? std::atoi(req.get_param_value("depth").c_str()) : 2;
        int k = req.has_param("k") ? std::atoi(req.get_param_value("k").c_str()) : 10;
        std::string pred = req.has_param("predicate") ? req.get_param_value("predicate") : "/pred/calls";
        bool no_content = req.has_param("no_content");
        long max_bytes = req.has_param("max_code_bytes")
                             ? std::atol(req.get_param_value("max_code_bytes").c_str())
                             : 0;
        try {
            Pool::Lease lease(pool);
            json_t* r = api_context(lease.db(), key, depth, k, pred, opt.vector_cmd, no_content, max_bytes);
            set_json(res, 200, dump(r, JSON_INDENT(2)));
            json_decref(r);
        } catch (const std::exception& e) {
            fail(res, 400, e.what());
        }
    });

    fprintf(stderr, "[SERVE] listening on %s:%d (pool=%d, ro=%d)\n", opt.bind.c_str(), opt.port,
            opt.pool, opt.read_only ? 1 : 0);
    if (!svr.listen(opt.bind.c_str(), opt.port)) {
        fprintf(stderr, "[SERVE] failed to bind %s:%d\n", opt.bind.c_str(), opt.port);
        return 1;
    }
    return 0;
}
