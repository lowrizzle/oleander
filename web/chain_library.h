#ifndef CHAIN_LIBRARY_H
#define CHAIN_LIBRARY_H

#include "web/preset_store.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// An unbounded, name-keyed library of saved chains, separate from the 5
// fixed preset slots PresetStore manages. A slot holds "whatever is
// currently assigned to switch N"; a library entry is a chain that exists
// on its own, independent of any slot, and can be assigned into any slot
// (or none) at any time. Reuses PresetStore's Preset struct (a name plus a
// pedal list) since a library entry is exactly that shape. See
// docs/ROADMAP.md item 2.
//
// Same persistence pattern as PresetStore: every mutation re-persists the
// whole library synchronously to its own JSON file (temp-file + rename),
// which is safe here for the same reason it is in PresetStore -- this is
// only ever called from HTTP handler threads on human-triggered events, not
// from the real-time audio callback.
class ChainLibrary {
public:
  explicit ChainLibrary(std::string path) : path_(std::move(path)) { Load(); }

  // Returns a copy of every entry, in library order (oldest save first;
  // stable across upserts of already-existing entries).
  std::vector<Preset> GetAll() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_;
  }

  // Copies the entry named `name` into `*out` and returns true, or returns
  // false (leaving `*out` untouched) if no entry has that name.
  bool Get(const std::string& name, Preset* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    int index = FindLocked(name);
    if (index < 0) {
      return false;
    }
    *out = entries_[index];
    return true;
  }

  // Inserts `entry` as a new library entry, or -- if an entry with the same
  // name already exists -- overwrites its pedal list in place (keeping its
  // existing position in the list). No-op (returns false) if the name is
  // empty.
  bool Upsert(Preset entry) {
    std::lock_guard<std::mutex> lock(mu_);
    if (entry.name.empty()) {
      return false;
    }
    int index = FindLocked(entry.name);
    if (index >= 0) {
      entries_[index] = std::move(entry);
    } else {
      entries_.push_back(std::move(entry));
    }
    PersistLocked();
    return true;
  }

  // Removes the entry named `name`. Returns false (no-op) if not found.
  bool Delete(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    int index = FindLocked(name);
    if (index < 0) {
      return false;
    }
    entries_.erase(entries_.begin() + index);
    PersistLocked();
    return true;
  }

private:
  // Caller must hold mu_.
  int FindLocked(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); i++) {
      if (entries_[i].name == name) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  void Load() {
    std::lock_guard<std::mutex> lock(mu_);
    std::ifstream file(path_);
    if (!file) {
      // No library file yet -- first run. Nothing to load.
      return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto parsed = crow::json::load(buffer.str());
    if (!parsed) {
      // Malformed/empty file -- start fresh rather than crash the server.
      return;
    }
    if (parsed.has("entries") &&
        parsed["entries"].t() == crow::json::type::List) {
      auto entries_json = parsed["entries"];
      for (size_t i = 0; i < entries_json.size(); i++) {
        entries_.push_back(DeserializePreset(entries_json[i]));
      }
    }
  }

  // Caller must hold mu_.
  void PersistLocked() const {
    crow::json::wvalue root;
    std::vector<crow::json::wvalue> entries_json;
    for (const auto& entry : entries_) {
      entries_json.push_back(SerializePreset(entry));
    }
    root["entries"] = std::move(entries_json);

    // Write to a temp file and rename over the real one, so a crash or
    // power loss mid-write can't leave chain_library.json truncated/corrupt.
    const std::string tmp_path = path_ + ".tmp";
    std::ofstream file(tmp_path, std::ios::trunc);
    if (!file) {
      return;
    }
    file << root.dump();
    file.close();
    std::rename(tmp_path.c_str(), path_.c_str());
  }

  mutable std::mutex mu_;
  std::string path_;
  std::vector<Preset> entries_;
};

#endif /* CHAIN_LIBRARY_H */
