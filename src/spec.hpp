#pragma once
#include "db.hpp"

#include <jansson.h>

#include <string>

// Store/load a spec entry at /spec/{category} (schema-as-data).
void spec_set(Db& db, const std::string& category, const std::string& spec_json);

// Returns a new reference to the spec content object, or nullptr if none.
json_t* spec_load(Db& db, const std::string& category);

// Validate an entry ({key, meta, content}) against a spec ({"required":[...], "types":{...}}).
void spec_validate(const json_t* spec, const json_t* entry, const std::string& key);
