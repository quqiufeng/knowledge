#pragma once
#include "compile.hpp"

#include <string>
#include <vector>

std::vector<VectorHit> run_vector_provider(const std::string& cmd, const std::string& query, int k);
