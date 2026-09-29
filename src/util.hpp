#pragma once
#include <jansson.h>

#include <fstream>
#include <string>

namespace kutil {

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
