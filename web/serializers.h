#ifndef SERIALIZERS_H
#define SERIALIZERS_H

#include "crow/json.h"
#include "pedal.h"

crow::json::wvalue SerializePedalKnob(const PedalKnob& pedal_knob) {
  crow::json::wvalue serialized;
  serialized["name"] = pedal_knob.name;
  serialized["value"] = pedal_knob.value;
  serialized["tweak_amount"] = pedal_knob.tweak_amount;
  serialized["min"] = pedal_knob.min;
  serialized["max"] = pedal_knob.max;
  // Omitted entirely (rather than an empty list) when there are no
  // labels, so the frontend's "is this a fader or an LED button group"
  // check is just presence-of-key.
  if (!pedal_knob.labels.empty()) {
    std::vector<crow::json::wvalue> labels;
    for (const auto& label : pedal_knob.labels) {
      labels.push_back(label);
    }
    serialized["labels"] = std::move(labels);
  }
  return serialized;
}

crow::json::wvalue SerializePedalInfo(const PedalInfo& pedal_info) {
  crow::json::wvalue serialized;
  // Stable id -- always address this pedal by `id` in follow-up requests
  // (adjust/remove/push), not by its position in the returned list, since
  // that position shifts whenever any pedal is added or removed.
  serialized["id"] = pedal_info.id;
  serialized["name"] = pedal_info.name;
  serialized["state"] = pedal_info.state;

  std::vector<crow::json::wvalue> knobs;
  for (const auto& knob : pedal_info.knobs) {
    knobs.push_back(SerializePedalKnob(knob));
  }
  serialized["knobs"] = std::move(knobs);

  return serialized;
}

#endif /* SERIALIZERS_H */
