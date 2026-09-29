#pragma once
#include "db.hpp"

#include <string>
#include <vector>

// Action: append an episodic memory event and link it to related entities.
std::string memory_remember(Db& db, const std::string& agent, const std::string& session,
                            const std::string& text, const std::vector<std::string>& about,
                            const std::vector<std::string>& tags);

// Action: upsert a keyed fact; the previous value is archived (history is data).
std::string memory_fact(Db& db, const std::string& agent, const std::string& topic,
                        const std::string& value_json, const std::string& session);

// Action: controlled link between two existing keys.
long memory_link(Db& db, const std::string& subject, const std::string& predicate,
                 const std::string& object);

// Action: forget = archive (never physical delete). Only /mem/ keys unless force.
bool memory_forget(Db& db, const std::string& key, bool force);
