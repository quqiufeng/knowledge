#pragma once
#include "db.hpp"

#include <string>

// Append an audit entry /audit/{agent}/{ts}-{pid}-{seq} and link it to the target.
std::string audit(Db& db, const std::string& agent, const std::string& action,
                  const std::string& target, const std::string& detail_json);

// Action: validated upsert of an entry (optionally against /spec/{category}).
std::string action_put(Db& db, const std::string& agent, const std::string& key,
                       const std::string& meta_json, const std::string& content_json,
                       const std::string& category, const std::string& text);

// Action: archive (never delete). Non-/mem/ keys need force.
bool action_forget(Db& db, const std::string& agent, const std::string& key, bool force);

// Action: restore an archived entry.
bool action_restore(Db& db, const std::string& agent, const std::string& key);
