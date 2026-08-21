#ifndef HANDLERS_H
#define HANDLERS_H

#include "crow.h"
#include "pedal_registry.h"
#include "pedals/all_pedals.h"
#include "web/chain_library.h"
#include "web/pedal_board.h"
#include "web/preset_store.h"
#include "web/serializers.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class StaticFileHandler {
public:
  StaticFileHandler(const std::string& directory) : directory_(directory) {}

  crow::response operator()(const std::string& filename) const {
    // Protect against escaping the static file directory.
    if (filename.find("..") != std::string::npos) {
      return crow::response(400);
    }

    const auto file_path = directory_ + "/" + filename;
    std::ifstream file_stream(file_path);
    if (!file_stream) {
      return crow::response(404);
    }

    static constexpr int kReadFileBufferSize = 1024;
    char read_buffer[kReadFileBufferSize];
    std::string response_body;
    while (file_stream) {
      file_stream.read(read_buffer, kReadFileBufferSize);
      std::copy(read_buffer, read_buffer + file_stream.gcount(),
                std::back_inserter(response_body));
    }

    return crow::response(std::move(response_body));
  }

private:
  std::string directory_;
};

class AvailablePedalHandler {
public:
  crow::response operator()() const {
    const auto registered_pedals =
        PedalRegistry::GetInstance().GetRegisteredPedals();
    std::vector<std::string> pedal_names;
    for (const auto& pedal_info : registered_pedals) {
      pedal_names.push_back(pedal_info.name);
    }

    crow::json::wvalue response;
    response = pedal_names;
    return crow::response(response);
  }
};

class ActivePedalHandler {
public:
  ActivePedalHandler(const PedalBoard* pedal_board)
      : pedal_board_(pedal_board) {}

  crow::response operator()() const {
    std::vector<crow::json::wvalue> active_pedals;
    auto pedals = pedal_board_->GetPedals();
    for (const auto& pedal : pedals) {
      active_pedals.push_back(SerializePedalInfo(pedal));
    }

    crow::json::wvalue response;
    response["pedals"] = std::move(active_pedals);
    return response;
  }

private:
  const PedalBoard* pedal_board_;
};

class UpdatesHandler {
public:
  void RegisterConnection(crow::websocket::connection* connection) {
    std::lock_guard<std::mutex> lock(mu_);
    connections_.insert(connection);
  }

  void RemoveConnection(crow::websocket::connection* connection) {
    std::lock_guard<std::mutex> lock(mu_);
    connections_.erase(connection);
  }

  void OnUpdate() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<crow::websocket::connection*> connections_copy;
    for (auto* connection : connections_) {
      connections_copy.push_back(connection);
    }
    for (auto* connection : connections_copy) {
      try {
        connection->send_text("ping");
      } catch (...) {
        RemoveConnection(connection);
      }
    }
  }

private:
  std::mutex mu_;
  std::unordered_set<crow::websocket::connection*> connections_;
};

// Persists the live board as PresetStore's "current" snapshot and notifies
// connected clients (browser + hardware_service.py, both listening on
// /updates) that something changed. Every handler that mutates the board
// or the preset list calls this exactly once, after the mutation, so the
// two effects (survive a restart / update the UI+OLED live) always happen
// together.
class ChangeNotifier {
public:
  ChangeNotifier(PedalBoard* pedal_board, PresetStore* presets,
                 UpdatesHandler* updates)
      : pedal_board_(pedal_board), presets_(presets), updates_(updates) {}

  void NotifyBoardChanged() const {
    presets_->SetCurrent(pedal_board_->GetPedals());
    updates_->OnUpdate();
  }

  void NotifyPresetsChanged() const { updates_->OnUpdate(); }

  // Same signal as NotifyPresetsChanged (both just tell connected clients to
  // re-fetch) -- kept as a separate name so call sites read as "the chain
  // library changed" rather than "a preset slot changed", even though the
  // two currently do the same thing.
  void NotifyLibraryChanged() const { updates_->OnUpdate(); }

private:
  PedalBoard* pedal_board_;
  PresetStore* presets_;
  UpdatesHandler* updates_;
};

class RemovePedalHandler {
public:
  RemovePedalHandler(PedalBoard* pedal_board, ChangeNotifier notifier)
      : pedal_board_(pedal_board), notifier_(notifier) {}

  // `pedal_id` is the stable PedalInfo::id, not a chain position.
  crow::response operator()(int pedal_id) const {
    pedal_board_->RemovePedal(pedal_id);
    notifier_.NotifyBoardChanged();
    return crow::response(200);
  }

private:
  mutable PedalBoard* pedal_board_;
  ChangeNotifier notifier_;
};

class AdjustKnobHandler {
public:
  AdjustKnobHandler(PedalBoard* pedal_board, ChangeNotifier notifier)
      : pedal_board_(pedal_board), notifier_(notifier) {}

  // `pedal_id` is the stable PedalInfo::id, not a chain position.
  crow::response operator()(const crow::request& request,
                            int pedal_id) const {
    auto* name_param = request.url_params.get("name");
    auto* value_param = request.url_params.get("value");

    if (!name_param || !value_param) {
      return crow::response(400);
    }

    double value;
    try {
      size_t pos = 0;
      value = std::stod(value_param, &pos);
      if (pos != strlen(value_param)) {
        return crow::response(400);
      }
    } catch (const std::exception&) {
      return crow::response(400);
    }

    if (std::isinf(value) || std::isnan(value)) {
      return crow::response(400);
    }

    PedalKnob knob;
    knob.name = name_param;
    knob.value = value;
    pedal_board_->AdjustKnob(pedal_id, knob);
    notifier_.NotifyBoardChanged();
    return crow::response(200);
  }

private:
  mutable PedalBoard* pedal_board_;
  ChangeNotifier notifier_;
};

class PushButtonHandler {
public:
  PushButtonHandler(PedalBoard* pedal_board, ChangeNotifier notifier)
      : pedal_board_(pedal_board), notifier_(notifier) {}

  // `pedal_id` is the stable PedalInfo::id, not a chain position. This
  // mutes/unmutes a single pedal -- it is unrelated to /preset/<n>, which
  // recalls a whole saved chain. The physical footswitches call the preset
  // endpoint; this one backs the per-pedal ON/OFF button in the web UI.
  crow::response operator()(int pedal_id) const {
    pedal_board_->Push(pedal_id);
    notifier_.NotifyBoardChanged();
    return crow::response(200);
  }

private:
  mutable PedalBoard* pedal_board_;
  ChangeNotifier notifier_;
};

class AddPedalHandler {
public:
  AddPedalHandler(PedalBoard* pedal_board, ChangeNotifier notifier)
      : pedal_board_(pedal_board), notifier_(notifier) {}

  // `pedal_name` arrives as the raw `<string>` route segment -- unlike a
  // query-string value (e.g. /preset/<n>/save?name=...), crow does not
  // URL-decode route-captured path segments, so a name containing
  // characters the browser had to percent-encode (a space, for instance)
  // shows up here still encoded (e.g. "Sky%20Chive") and would otherwise
  // never match a registry key. Decode it the same way crow already
  // decodes query-string values (crow::qs_decode), rather than requiring
  // every pedal name to avoid such characters.
  crow::response operator()(const std::string& pedal_name) const {
    std::string decoded_name = pedal_name;
    if (!decoded_name.empty()) {
      int decoded_length = crow::qs_decode(&decoded_name[0]);
      decoded_name.resize(decoded_length);
    }

    auto pedal_factory =
        PedalRegistry::GetInstance().GetPedalFactoryOrNull(decoded_name);
    if (!pedal_factory) {
      return crow::response(404);
    }

    pedal_board_->AddPedal((*pedal_factory)());
    notifier_.NotifyBoardChanged();
    return crow::response(200);
  }

private:
  mutable PedalBoard* pedal_board_;
  ChangeNotifier notifier_;
};

// GET /presets -- lists all 5 preset slots (name + pedal count) plus which
// one, if any, is currently active. Backs the presets bar in the web UI.
class PresetListHandler {
public:
  explicit PresetListHandler(PresetStore* presets) : presets_(presets) {}

  crow::response operator()() const {
    auto all = presets_->GetAll();
    std::vector<crow::json::wvalue> presets_json;
    for (size_t i = 0; i < all.size(); i++) {
      crow::json::wvalue entry;
      entry["index"] = static_cast<int>(i);
      entry["name"] = all[i].name;
      entry["pedal_count"] = static_cast<int>(all[i].pedals.size());
      presets_json.push_back(std::move(entry));
    }

    crow::json::wvalue response;
    response["presets"] = std::move(presets_json);
    response["active_index"] = presets_->GetActiveIndex();
    return response;
  }

private:
  mutable PresetStore* presets_;
};

// GET /preset/<n> -- recalls preset slot n (0-4) into the live board. This
// is what the 5 physical footswitches call, so it is debounced server-side
// against duplicate requests arriving within a short window of each other
// (e.g. from a bouncy mechanical switch, or two requests racing in from
// the web UI and a footswitch at once) -- a duplicate is treated as a
// no-op success rather than reloading (and re-broadcasting) the same
// preset twice.
class LoadPresetHandler {
public:
  LoadPresetHandler(PedalBoard* pedal_board, PresetStore* presets,
                     ChangeNotifier notifier)
      : pedal_board_(pedal_board), presets_(presets), notifier_(notifier),
        debounce_(std::make_shared<DebounceState>()) {}

  crow::response operator()(int index) const {
    if (index < 0 || index >= PresetStore::kNumPresets) {
      return crow::response(400);
    }

    Preset preset;
    if (!presets_->Get(index, &preset)) {
      return crow::response(404);
    }

    {
      std::lock_guard<std::mutex> lock(debounce_->mu);
      const auto now = std::chrono::steady_clock::now();
      if (index == debounce_->last_index &&
          now - debounce_->last_load_time < std::chrono::milliseconds(150)) {
        return crow::response(200);
      }
      debounce_->last_index = index;
      debounce_->last_load_time = now;
    }

    pedal_board_->LoadSnapshot(preset.pedals);
    presets_->SetActiveIndex(index);
    notifier_.NotifyBoardChanged();
    return crow::response(200);
  }

private:
  // Debounce state lives behind a shared_ptr rather than as a direct
  // std::mutex member: Crow moves route handler objects into its internal
  // dispatch closures (`[f = std::move(f)]`), which requires the handler
  // to stay movable/copyable. A std::mutex member would make that
  // implicitly deleted; a shared_ptr to a heap-allocated state block is
  // both copyable and keeps every copy pointing at the same debounce
  // state, which is what we want regardless of how many times Crow
  // copies/moves this object internally.
  struct DebounceState {
    std::mutex mu;
    int last_index = -1;
    std::chrono::steady_clock::time_point last_load_time;
  };

  mutable PedalBoard* pedal_board_;
  mutable PresetStore* presets_;
  ChangeNotifier notifier_;
  std::shared_ptr<DebounceState> debounce_;
};

// POST /preset/<n>/save[?name=...] -- snapshots the current live board into
// preset slot n, optionally renaming it. `name` (if present) is read as a
// URL query parameter, matching the convention already used by
// /adjust_knob -- e.g. POST /preset/0/save?name=Lead%20Boost.
//
// Every save also upserts a same-named entry into the chain library
// (independent of, and outliving, whichever slot it was saved from) -- see
// docs/ROADMAP.md item 2. This is the library's only write path; there is
// deliberately no separate "save to library" endpoint or opt-in step.
class SavePresetHandler {
public:
  SavePresetHandler(PedalBoard* pedal_board, PresetStore* presets,
                     ChainLibrary* library, ChangeNotifier notifier)
      : pedal_board_(pedal_board), presets_(presets), library_(library),
        notifier_(notifier) {}

  crow::response operator()(const crow::request& request, int index) const {
    if (index < 0 || index >= PresetStore::kNumPresets) {
      return crow::response(400);
    }

    Preset preset;
    preset.pedals = pedal_board_->GetPedals();
    auto* name_param = request.url_params.get("name");
    if (name_param != nullptr && strlen(name_param) > 0) {
      preset.name = name_param;
    }

    if (!presets_->Save(index, preset)) {
      return crow::response(400);
    }

    // Re-fetch rather than reuse `preset`: if name_param was empty,
    // PresetStore::Save resolved the name to the slot's existing name
    // internally (on its own copy), which the local `preset` here never
    // saw -- this is what the library entry should be upserted under.
    Preset saved;
    presets_->Get(index, &saved);
    library_->Upsert(saved);

    notifier_.NotifyPresetsChanged();
    return crow::response(200);
  }

private:
  mutable PedalBoard* pedal_board_;
  mutable PresetStore* presets_;
  mutable ChainLibrary* library_;
  ChangeNotifier notifier_;
};

// POST /preset/<n>/name?name=... -- renames preset slot n without touching
// its saved pedals.
class RenamePresetHandler {
public:
  RenamePresetHandler(PresetStore* presets, ChangeNotifier notifier)
      : presets_(presets), notifier_(notifier) {}

  crow::response operator()(const crow::request& request, int index) const {
    auto* name_param = request.url_params.get("name");
    if (name_param == nullptr || strlen(name_param) == 0) {
      return crow::response(400);
    }
    if (!presets_->Rename(index, name_param)) {
      return crow::response(400);
    }
    notifier_.NotifyPresetsChanged();
    return crow::response(200);
  }

private:
  mutable PresetStore* presets_;
  ChangeNotifier notifier_;
};

// GET /chain_library -- lists every saved chain (name + pedal count), in
// library order. Backs the chain-library list on the main page. Unlike
// /presets, there is no "active" entry -- library entries aren't loaded
// onto the live board directly, only assigned into a slot (see
// ChainLibraryAssignHandler below) or deleted.
class ChainLibraryListHandler {
public:
  explicit ChainLibraryListHandler(ChainLibrary* library) : library_(library) {}

  crow::response operator()() const {
    auto all = library_->GetAll();
    std::vector<crow::json::wvalue> entries_json;
    for (const auto& entry : all) {
      crow::json::wvalue json_entry;
      json_entry["name"] = entry.name;
      json_entry["pedal_count"] = static_cast<int>(entry.pedals.size());
      entries_json.push_back(std::move(json_entry));
    }
    crow::json::wvalue response;
    response["entries"] = std::move(entries_json);
    return response;
  }

private:
  mutable ChainLibrary* library_;
};

// POST /chain_library/<name>/delete -- removes a library entry. Does not
// touch any preset slot it may have previously been assigned into; slots
// hold their own copy of the pedal list, independent of the library entry
// it came from.
class ChainLibraryDeleteHandler {
public:
  ChainLibraryDeleteHandler(ChainLibrary* library, ChangeNotifier notifier)
      : library_(library), notifier_(notifier) {}

  // `name` arrives as the raw `<string>` route segment -- see
  // AddPedalHandler above for why this needs qs_decode.
  crow::response operator()(const std::string& name) const {
    std::string decoded_name = name;
    if (!decoded_name.empty()) {
      int decoded_length = crow::qs_decode(&decoded_name[0]);
      decoded_name.resize(decoded_length);
    }

    if (!library_->Delete(decoded_name)) {
      return crow::response(404);
    }
    notifier_.NotifyLibraryChanged();
    return crow::response(200);
  }

private:
  mutable ChainLibrary* library_;
  ChangeNotifier notifier_;
};

// POST /chain_library/<name>/assign/<slot> -- copies a library entry's
// pedal list into preset slot `slot` (0-4), the same way SavePresetHandler
// writes a slot, and renames that slot to the entry's name. Does not touch
// the live board -- assigning a slot is independent of what's currently
// loaded, exactly like re-saving a different slot is.
class ChainLibraryAssignHandler {
public:
  ChainLibraryAssignHandler(PresetStore* presets, ChainLibrary* library,
                             ChangeNotifier notifier)
      : presets_(presets), library_(library), notifier_(notifier) {}

  crow::response operator()(const std::string& name, int slot) const {
    if (slot < 0 || slot >= PresetStore::kNumPresets) {
      return crow::response(400);
    }

    std::string decoded_name = name;
    if (!decoded_name.empty()) {
      int decoded_length = crow::qs_decode(&decoded_name[0]);
      decoded_name.resize(decoded_length);
    }

    Preset entry;
    if (!library_->Get(decoded_name, &entry)) {
      return crow::response(404);
    }

    if (!presets_->Save(slot, entry)) {
      return crow::response(400);
    }
    notifier_.NotifyPresetsChanged();
    return crow::response(200);
  }

private:
  mutable PresetStore* presets_;
  mutable ChainLibrary* library_;
  ChangeNotifier notifier_;
};

// In-memory latch state for the 5 physical footswitches (indices 0-4,
// matching the preset slots they recall -- see
// hardware/hardware_service.py's PRESET_GPIO_MAP). Not persisted to disk:
// this mirrors live GPIO pin state, which hardware_service.py re-reports
// on every (re)start anyway, so there is nothing meaningful to save across
// a server restart. This is separate from PresetStore's active_index --
// a switch's physical latch position and "which preset is currently
// loaded" are related but distinct (e.g. the board can be freely edited
// away from any preset while a switch is still sitting in whatever
// position it was last pressed to).
class SwitchStates {
public:
  static constexpr int kNumSwitches = 5;

  // Returns a copy of all switch states (true = currently latched on).
  std::vector<bool> GetAll() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::vector<bool>(pressed_.begin(), pressed_.end());
  }

  // Sets switch `index`'s latch state. Returns false (no-op) if `index` is
  // out of range.
  bool Set(int index, bool pressed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kNumSwitches) {
      return false;
    }
    pressed_[index] = pressed;
    return true;
  }

private:
  mutable std::mutex mu_;
  std::array<bool, kNumSwitches> pressed_{};
};

// GET /switches -- reports the live latch state of all 5 physical
// footswitches (true = currently latched on), as reported by
// hardware_service.py. Backs the on/off indicator dot on each preset tile
// in the web UI -- useful feedback in its own right, and especially so
// while the OLED display isn't working (not wired up yet, or I2C isn't
// enabled -- see codefix.md Round 5 #1), since it's otherwise the only
// visible confirmation that a footswitch press was actually registered.
class SwitchListHandler {
public:
  explicit SwitchListHandler(const SwitchStates* switches)
      : switches_(switches) {}

  crow::response operator()() const {
    auto all = switches_->GetAll();
    std::vector<crow::json::wvalue> switches_json;
    for (size_t i = 0; i < all.size(); i++) {
      crow::json::wvalue entry;
      entry["index"] = static_cast<int>(i);
      entry["pressed"] = static_cast<bool>(all[i]);
      switches_json.push_back(std::move(entry));
    }
    crow::json::wvalue response;
    response["switches"] = std::move(switches_json);
    return response;
  }

private:
  const SwitchStates* switches_;
};

// POST /switch/<n>/state?pressed=<0|1> -- reports switch `n`'s current
// physical latch state. Called by hardware/hardware_service.py on every
// press/release, and once at startup for each switch's actual initial
// position, so the web UI (and anyone loading/reloading the page) always
// reflects what the hardware is actually doing right now -- independent
// of, and in addition to, the preset recall a press also triggers via
// /preset/<n>.
class SwitchStateHandler {
public:
  SwitchStateHandler(SwitchStates* switches, UpdatesHandler* updates)
      : switches_(switches), updates_(updates) {}

  crow::response operator()(const crow::request& request, int index) const {
    auto* pressed_param = request.url_params.get("pressed");
    if (!pressed_param) {
      return crow::response(400);
    }
    const std::string pressed_str(pressed_param);
    bool pressed;
    if (pressed_str == "1" || pressed_str == "true") {
      pressed = true;
    } else if (pressed_str == "0" || pressed_str == "false") {
      pressed = false;
    } else {
      return crow::response(400);
    }

    if (!switches_->Set(index, pressed)) {
      return crow::response(400);
    }
    updates_->OnUpdate();
    return crow::response(200);
  }

private:
  mutable SwitchStates* switches_;
  UpdatesHandler* updates_;
};

#endif /* HANDLERS_H */
