#pragma once
#include <string>
#include <vector>

std::vector<std::string> split_identifier(const std::string& word);
std::vector<std::string> tokenize_code(const std::string& text);
std::string build_ts_query(const std::vector<std::string>& tokens);
std::string to_tsvector_text(const std::vector<std::string>& tokens);
