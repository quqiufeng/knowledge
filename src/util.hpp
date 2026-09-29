#pragma once
#include <jansson.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace kutil {

struct JsonDeleter {
    void operator()(json_t* p) const {
        if (p) json_decref(p);
    }
};
using json_ptr = std::unique_ptr<json_t, JsonDeleter>;

inline std::string now_iso() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::string json_as_text(const json_t* v) {
    if (!v) return "";
    if (json_is_string(v)) return json_string_value(v);
    char* s = json_dumps(v, JSON_COMPACT);
    std::string out = s ? s : "";
    if (s) free(s);
    return out;
}

inline long long json_as_int(const json_t* v, long long def = 0) {
    if (!v) return def;
    if (json_is_integer(v)) return json_integer_value(v);
    if (json_is_string(v)) return std::atoll(json_string_value(v));
    if (json_is_real(v)) return static_cast<long long>(json_real_value(v));
    return def;
}

inline double json_as_num(const json_t* v, double def = 0.0) {
    if (!v) return def;
    if (json_is_real(v)) return json_real_value(v);
    if (json_is_integer(v)) return static_cast<double>(json_integer_value(v));
    if (json_is_string(v)) return std::atof(json_string_value(v));
    return def;
}

inline std::string escape_like(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '%' || c == '_') out += '\\';
        out += c;
    }
    return out;
}

// Postgres text[] literal: {a,"b c"} (escapes " and \).
inline std::string pg_text_array(const std::vector<std::string>& items) {
    std::string out = "{";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += '"';
        for (char c : items[i]) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
    }
    out += "}";
    return out;
}

inline std::string escape_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::string relpath_of(const std::string& file, const std::string& root) {
    if (!root.empty() && file.rfind(root, 0) == 0) {
        std::string r = file.substr(root.size());
        if (!r.empty() && r[0] == '/') r = r.substr(1);
        return r;
    }
    return (!file.empty() && file[0] == '/') ? file.substr(1) : file;
}

inline std::string dump_owned(json_t* o) {
    char* s = json_dumps(o, JSON_COMPACT);
    std::string out = s ? s : "";
    if (s) free(s);
    json_decref(o);
    return out;
}

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

}  // namespace kutil
