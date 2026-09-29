#include "tokenize.hpp"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

const std::unordered_set<std::string>& stop_words() {
    static const std::unordered_set<std::string> s = {
        "the", "and", "for", "with", "this", "that", "from", "into", "return",
        "void", "int", "char", "long", "unsigned", "signed", "const", "static",
        "struct", "union", "enum", "typedef", "sizeof", "if", "else", "while",
        "do", "switch", "case", "break", "continue", "goto", "default", "new",
        "delete", "true", "false", "null", "nullptr", "self", "def", "class",
        "public", "private", "protected", "function", "var", "let", "export",
    };
    return s;
}

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_start(char c) { return is_lower(c) || is_upper(c) || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

void split_camel(const std::string& part, std::vector<std::string>& out) {
    std::string cur;
    size_t n = part.size();
    for (size_t i = 0; i < n; ++i) {
        char c = part[i];
        if (!cur.empty()) {
            char prev = part[i - 1];
            bool boundary = false;
            if (is_upper(c) && (is_lower(prev) || is_digit(prev))) boundary = true;
            if (is_upper(c) && is_upper(prev) && i + 1 < n && is_lower(part[i + 1])) boundary = true;
            if (boundary) {
                out.push_back(cur);
                cur.clear();
            }
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
}

}  // namespace

std::vector<std::string> split_identifier(const std::string& word) {
    std::vector<std::string> out;
    std::string part;
    auto flush = [&]() {
        if (!part.empty()) {
            std::vector<std::string> camel;
            split_camel(part, camel);
            for (auto& c : camel) out.push_back(lower(c));
            part.clear();
        }
    };
    for (char c : word) {
        if (c == '_') {
            flush();
        } else {
            part.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::string> tokenize_code(const std::string& text) {
    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& t) {
        if (t.size() > 1 && !stop_words().count(t) && seen.insert(t).second) tokens.push_back(t);
    };

    size_t i = 0, n = text.size();
    while (i < n) {
        char c = text[i];
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident_char(text[j])) ++j;
            std::string word = text.substr(i, j - i);
            add(lower(word));
            for (auto& p : split_identifier(word)) add(p);
            i = j;
        } else if (is_digit(c)) {
            size_t j = i;
            while (j < n && is_digit(text[j])) ++j;
            add(text.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    return tokens;
}

std::string build_ts_query(const std::vector<std::string>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out += " | ";
        out += tokens[i];
    }
    return out;
}

std::string to_tsvector_text(const std::vector<std::string>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out.push_back(' ');
        out += tokens[i];
    }
    return out;
}
