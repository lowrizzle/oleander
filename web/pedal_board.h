#ifndef PEDAL_BOARD_H
#define PEDAL_BOARD_H

#include "pedal.h"
#include "pedal_registry.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class PedalBoard : public Pedal {
public:
  void AddPedal(std::unique_ptr<Pedal> pedal) {
    std::lock_guard<std::mutex> lock(mutex_);
    pedals_.push_back(PedalEntry{next_id_++, std::move(pedal)});
  }

  SignalType Transform(SignalType input) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : pedals_) {
      if (entry.pedal->Enabled()) {
        input = entry.pedal->Transform(input);
      }
    }
    return input;
  }

  // NOTE: `pedal_id` below is the stable PedalInfo::id returned by
  // GetPedals(), *not* a position in the chain. Chain position shifts
  // every time a pedal is added or removed, so an index captured a moment
  // ago may silently refer to a different pedal by the time a request
  // using it arrives. Callers should always look up the id from the most
  // recent GetPedals()/`/active_pedals` response.

  void AdjustKnob(int pedal_id, const PedalKnob& knob) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* entry = FindByIdLocked(pedal_id);
    if (entry == nullptr) {
      return;
    }
    entry->pedal->AdjustKnob(knob);
  }

  void RemovePedal(int pedal_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(pedals_.begin(), pedals_.end(),
                            [pedal_id](const PedalEntry& entry) {
                              return entry.id == pedal_id;
                            });
    if (it == pedals_.end()) {
      return;
    }
    pedals_.erase(it);
  }

  void Push(int pedal_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* entry = FindByIdLocked(pedal_id);
    if (entry == nullptr) {
      return;
    }
    entry->pedal->Push();
  }

  PedalInfo Describe() override {
    std::lock_guard<std::mutex> lock(mutex_);
    PedalInfo info;
    info.name = "PedalBoard";
    info.state = std::to_string(pedals_.size()) + " pedal(s)";
    return info;
  }

  std::vector<PedalInfo> GetPedals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PedalInfo> pedals;
    pedals.reserve(pedals_.size());
    for (const auto& entry : pedals_) {
      PedalInfo info = entry.pedal->Describe();
      info.id = entry.id;
      info.state = entry.pedal->State();
      pedals.push_back(std::move(info));
    }
    return pedals;
  }

  // Replaces the entire chain with pedals reconstructed from `snapshot`
  // (the same shape GetPedals() returns -- e.g. a saved preset, or the
  // last-known "current" board loaded at startup). Each pedal gets a
  // freshly assigned id; the ids in `snapshot` (if any) are ignored, since
  // this is rebuilding new pedal instances, not reusing old ones.
  //
  // A pedal type that is no longer registered (e.g. a preset saved by an
  // older build referencing a since-removed pedal) is skipped rather than
  // failing the whole load, so the rest of the preset still comes back.
  void LoadSnapshot(const std::vector<PedalInfo>& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PedalEntry> new_pedals;
    new_pedals.reserve(snapshot.size());
    for (const auto& info : snapshot) {
      auto* factory = PedalRegistry::GetInstance().GetPedalFactoryOrNull(info.name);
      if (factory == nullptr) {
        continue;
      }
      auto pedal = (*factory)();
      for (const auto& knob : info.knobs) {
        pedal->AdjustKnob(knob);
      }
      pedal->SetEnabled(info.state != "Disabled");
      new_pedals.push_back(PedalEntry{next_id_++, std::move(pedal)});
    }
    pedals_ = std::move(new_pedals);
  }

private:
  struct PedalEntry {
    int id;
    std::unique_ptr<Pedal> pedal;
  };

  // Caller must hold mutex_.
  PedalEntry* FindByIdLocked(int pedal_id) {
    for (auto& entry : pedals_) {
      if (entry.id == pedal_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  mutable std::mutex mutex_;
  std::vector<PedalEntry> pedals_;
  int next_id_ = 0;
};

#endif /* PEDAL_BOARD_H */
