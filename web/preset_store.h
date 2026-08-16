#ifndef PRESET_STORE_H
#define PRESET_STORE_H

#include "pedal.h"
#include "web/serializers.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// A saved snapshot of a pedal board: an ordered list of pedals with their
// knob values and enabled/disabled state, plus a human-readable name. This
// is exactly the shape PedalBoard::GetPedals() already returns -- a preset
// is just a copy of that, kept around and given a name.
struct Preset {
  std::string name;
  std::vector<PedalInfo> pedals;
};

crow::json::wvalue SerializePreset(const Preset& preset) {
  crow::json::wvalue serialized;
  serialized["name"] = preset.name;
  std::vector<crow::json::wvalue> pedals;
  for (const auto& pedal : preset.pedals) {
    pedals.push_back(SerializePedalInfo(pedal));
  }
  serialized["pedals"] = std::move(pedals);
  return serialized;
}

Preset DeserializePreset(const crow::json::rvalue& value) {
  Preset preset;
  if (!value || value.t() != crow::json::type::Object) {
    return preset;
  }
  if (value.has("name")) {
    preset.name = static_cast<std::string>(value["name"].s());
  }
  if (value.has("pedals") && value["pedals"].t() == crow::json::type::List) {
    auto pedals_json = value["pedals"];
    for (size_t i = 0; i < pedals_json.size(); i++) {
      const auto& pedal_json = pedals_json[i];
      PedalInfo info;
      if (pedal_json.has("id")) {
        info.id = static_cast<int>(pedal_json["id"].i());
      }
      if (pedal_json.has("name")) {
        info.name = static_cast<std::string>(pedal_json["name"].s());
      }
      if (pedal_json.has("state")) {
        info.state = static_cast<std::string>(pedal_json["state"].s());
      }
      if (pedal_json.has("knobs") && pedal_json["knobs"].t() == crow::json::type::List) {
        auto knobs_json = pedal_json["knobs"];
        for (size_t k = 0; k < knobs_json.size(); k++) {
          const auto& knob_json = knobs_json[k];
          PedalKnob knob{};
          if (knob_json.has("name")) {
            knob.name = static_cast<std::string>(knob_json["name"].s());
          }
          if (knob_json.has("value")) {
            knob.value = knob_json["value"].d();
          }
          if (knob_json.has("tweak_amount")) {
            knob.tweak_amount = knob_json["tweak_amount"].d();
          }
          info.knobs.push_back(std::move(knob));
        }
      }
      preset.pedals.push_back(std::move(info));
    }
  }
  return preset;
}

// Persists 5 named preset slots, plus the current (possibly unsaved) board
// state and which slot -- if any -- it corresponds to, in a single JSON
// file on disk. This is what lets presets, and whatever patch you currently
// have dialed in, survive a power cycle instead of coming back to an empty
// board.
//
// All public methods are thread-safe; every mutating call re-persists the
// whole store to disk immediately (this is only ever called from HTTP
// handler threads on human-triggered events -- add/remove/adjust a pedal,
// press a footswitch -- never from the real-time audio callback, so a
// synchronous file write here is not a performance concern).
class PresetStore {
public:
  static constexpr int kNumPresets = 5;

  explicit PresetStore(std::string path) : path_(std::move(path)) {
    presets_.resize(kNumPresets);
    for (int i = 0; i < kNumPresets; i++) {
      presets_[i].name = "Preset " + std::to_string(i + 1);
    }
    Load();
  }

  // Returns a copy of all preset slots.
  std::vector<Preset> GetAll() const {
    std::lock_guard<std::mutex> lock(mu_);
    return presets_;
  }

  // Copies preset `index` into `*out` and returns true, or returns false
  // (leaving `*out` untouched) if `index` is out of range.
  bool Get(int index, Preset* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kNumPresets) {
      return false;
    }
    *out = presets_[index];
    return true;
  }

  // Saves `preset` into slot `index`, marks that slot active, and persists.
  // If `preset.name` is empty, the slot keeps its existing name.
  bool Save(int index, Preset preset) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kNumPresets) {
      return false;
    }
    if (preset.name.empty()) {
      preset.name = presets_[index].name;
    }
    presets_[index] = std::move(preset);
    active_index_ = index;
    PersistLocked();
    return true;
  }

  bool Rename(int index, const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kNumPresets || name.empty()) {
      return false;
    }
    presets_[index].name = name;
    PersistLocked();
    return true;
  }

  // Records `pedals` as the current (possibly-unsaved) board state, so a
  // restart resumes with whatever was last dialed in.
  void SetCurrent(std::vector<PedalInfo> pedals) {
    std::lock_guard<std::mutex> lock(mu_);
    current_.name = "Current";
    current_.pedals = std::move(pedals);
    PersistLocked();
  }

  Preset GetCurrent() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

  // Marks preset slot `index` (or -1 for "no preset / freely edited") as
  // the one currently active, for the UI's "which preset is this" highlight.
  void SetActiveIndex(int index) {
    std::lock_guard<std::mutex> lock(mu_);
    active_index_ = index;
    PersistLocked();
  }

  int GetActiveIndex() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_index_;
  }

private:
  void Load() {
    std::lock_guard<std::mutex> lock(mu_);
    std::ifstream file(path_);
    if (!file) {
      // No presets file yet -- first run. Nothing to load.
      return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto parsed = crow::json::load(buffer.str());
    if (!parsed) {
      // Malformed/empty file -- start fresh rather than crash the server.
      return;
    }

    if (parsed.has("presets") && parsed["presets"].t() == crow::json::type::List) {
      auto presets_json = parsed["presets"];
      for (size_t i = 0; i < presets_json.size() && i < static_cast<size_t>(kNumPresets); i++) {
        presets_[i] = DeserializePreset(presets_json[i]);
      }
    }
    if (parsed.has("current")) {
      current_ = DeserializePreset(parsed["current"]);
    }
    if (parsed.has("active_index")) {
      active_index_ = static_cast<int>(parsed["active_index"].i());
    }
  }

  // Caller must hold mu_.
  void PersistLocked() const {
    crow::json::wvalue root;
    std::vector<crow::json::wvalue> presets_json;
    for (const auto& preset : presets_) {
      presets_json.push_back(SerializePreset(preset));
    }
    root["presets"] = std::move(presets_json);
    root["current"] = SerializePreset(current_);
    root["active_index"] = active_index_;

    // Write to a temp file and rename over the real one, so a crash or
    // power loss mid-write can't leave presets.json truncated/corrupt.
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
  std::vector<Preset> presets_;
  Preset current_;
  int active_index_ = -1;
};

#endif /* PRESET_STORE_H */
