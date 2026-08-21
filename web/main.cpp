#define CROW_MAIN

#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>

#include "visualization/visualizer.h"

#include "audio_transformer.h"
#include "crow.h"
#include "playback.h"
#include "rtaudio/RtAudio.h"
#include "web/handlers.h"
#include "web/pedal_board.h"
#include "web/preset_store.h"

struct AudioDevice {
  unsigned int device_index;
  RtAudio::DeviceInfo info;
};

std::vector<AudioDevice> GetAllDevices() {
  std::vector<AudioDevice> devices;
  RtAudio audio_interface;
  unsigned int num_devices = audio_interface.getDeviceCount();
  for (unsigned int i = 0; i < num_devices; i++) {
    auto info = audio_interface.getDeviceInfo(i);
    if (!info.probed) {
      continue;
    }
    devices.push_back(AudioDevice{.device_index = i, .info = info});
  }
  return devices;
}

std::vector<AudioDevice>
FilterDevices(const std::vector<AudioDevice>& devices,
              const std::function<bool(const AudioDevice&)> predicate) {
  std::vector<AudioDevice> filtered_devices;
  for (const auto& device : devices) {
    if (predicate(device)) {
      filtered_devices.push_back(device);
    }
  }
  return filtered_devices;
}

void ShowDevices(const std::vector<AudioDevice>& devices) {
  for (const auto& device : devices) {
    std::cout << device.device_index << " - " << device.info.name << std::endl;
  }
}

template <typename T> struct MaybeError {
  std::string error_message;
  T value;
};

struct DeviceSelections {
  AudioDevice input_device;
  AudioDevice output_device;
};

void WriteSelections(const DeviceSelections& selections) {
  std::ofstream devices_file("devices.txt");
  devices_file << selections.input_device.info.name << "\n"
               << selections.output_device.info.name;
}

MaybeError<DeviceSelections>
ReadSelectionsFromFile(const std::vector<AudioDevice>& all_devices) {
  std::ifstream devices_file("devices.txt");
  if (!devices_file) {
    return MaybeError<DeviceSelections>{.error_message =
                                            "Devices file not found"};
  }

  std::string input_device_name;
  std::getline(devices_file, input_device_name);
  auto input_device =
      FilterDevices(all_devices, [&input_device_name](const auto& device) {
        return device.info.inputChannels > 0 &&
               device.info.name == input_device_name;
      });
  if (input_device.empty()) {
    return MaybeError<DeviceSelections>{
        .error_message = "Input device '" + input_device_name + "' not found"};
  }

  std::string output_device_name;
  std::getline(devices_file, output_device_name);
  auto output_device =
      FilterDevices(all_devices, [&output_device_name](const auto& device) {
        return device.info.outputChannels > 0 &&
               device.info.name == output_device_name;
      });
  if (output_device.empty()) {
    return MaybeError<DeviceSelections>{.error_message = "output device '" +
                                                         output_device_name +
                                                         "' not found"};
  }

  return MaybeError<DeviceSelections>{
      .value = DeviceSelections{.input_device = input_device.front(),
                                .output_device = output_device.front()}};
}

// Picks a reasonable default without prompting anyone: the first
// input-capable device (there's rarely more than one on a headless pedal --
// the Pi's own onboard audio has no input channels at all, so this is
// almost always the USB interface), and for output, the *same* physical
// device if it also does output (the normal guitar-pedal setup: one
// interface does both directions) rather than defaulting to whichever
// output-capable device happens to enumerate first, which on a Pi is often
// its own onboard bcm2835 headphone jack -- silently sending the processed
// signal somewhere the player isn't listening.
MaybeError<DeviceSelections>
AutoSelectDevices(const std::vector<AudioDevice>& all_devices) {
  auto input_candidates = FilterDevices(all_devices, [](const auto& device) {
    return device.info.inputChannels > 0;
  });
  if (input_candidates.empty()) {
    return MaybeError<DeviceSelections>{
        .error_message = "No audio input device found"};
  }
  const AudioDevice& input_device = input_candidates.front();

  AudioDevice output_device = input_device;
  if (output_device.info.outputChannels == 0) {
    auto output_candidates =
        FilterDevices(all_devices, [](const auto& device) {
          return device.info.outputChannels > 0;
        });
    if (output_candidates.empty()) {
      return MaybeError<DeviceSelections>{
          .error_message = "No audio output device found"};
    }
    output_device = output_candidates.front();
  }

  return MaybeError<DeviceSelections>{
      .value = DeviceSelections{.input_device = input_device,
                                .output_device = output_device}};
}

DeviceSelections SelectDevices() {
  auto all_devices = GetAllDevices();
  MaybeError<DeviceSelections> file_selections =
      ReadSelectionsFromFile(all_devices);
  if (file_selections.error_message.empty()) {
    return file_selections.value;
  }

  std::cout << file_selections.error_message << std::endl;

  // No devices.txt yet. The code below this point used to unconditionally
  // prompt on std::cin, which only makes sense with a real terminal
  // attached. Under systemd (Type=simple, no TTY) stdin is closed/empty,
  // so `std::cin >> selected_input_device` hits EOF immediately rather
  // than blocking -- and because the *second* `std::cin >>` runs on a
  // stream whose failbit is already set from the first, it leaves
  // `selected_output_device` completely uninitialized (not even zeroed --
  // that only happens when extraction is attempted on a stream that was
  // still good beforehand). Indexing `all_devices` with that garbage value
  // is undefined behavior; in practice this showed up as a `std::bad_alloc`
  // crash-loop on first boot (reading a garbage std::string's length as if
  // it were real). See codefix.md Round 4 #1.
  //
  // Fix: only prompt interactively when a real terminal is attached (e.g.
  // running `./bin/server debug` by hand over SSH). Otherwise, auto-select
  // and persist the choice to devices.txt, so this only has to happen once
  // and every later boot goes straight through ReadSelectionsFromFile
  // above.
  if (!isatty(STDIN_FILENO)) {
    auto auto_selection = AutoSelectDevices(all_devices);
    if (!auto_selection.error_message.empty()) {
      std::cerr << "Fatal: " << auto_selection.error_message << ". Connect "
                << "your USB audio interface and let the service restart, "
                << "or run './bin/server debug' from a terminal (e.g. over "
                << "SSH) to pick a device by hand -- either way, the choice "
                << "is written to devices.txt so this only has to happen "
                << "once." << std::endl;
      std::exit(1);
    }
    std::cout << "No terminal attached to choose a device interactively -- "
              << "auto-selecting:" << std::endl;
    std::cout << "  Input:  " << auto_selection.value.input_device.info.name
              << std::endl;
    std::cout << "  Output: " << auto_selection.value.output_device.info.name
              << std::endl;
    WriteSelections(auto_selection.value);
    return auto_selection.value;
  }

  std::cout << "Available input devices:" << std::endl;
  ShowDevices(FilterDevices(all_devices, [](const AudioDevice& device) {
    return device.info.inputChannels > 0;
  }));
  int selected_input_device;
  std::cout << "> ";
  std::cin >> selected_input_device;

  std::cout << "Available output devices:" << std::endl;
  ShowDevices(FilterDevices(all_devices, [](const AudioDevice& device) {
    return device.info.outputChannels > 0;
  }));
  int selected_output_device;
  std::cout << "> ";
  std::cin >> selected_output_device;

  RtAudio audio_interface;
  DeviceSelections selections{
      .input_device = all_devices[selected_input_device],
      .output_device = all_devices[selected_output_device]};
  WriteSelections(selections);
  return selections;
}

int main(int argc, char* argv[]) {
  // `./bin/server list-devices` prints every audio device RtAudio detects
  // (id, name, input/output channel counts) and exits immediately -- no
  // audio stream opened, no web server started, no interactive prompt.
  // This is the easiest way to find the exact device name string
  // devices.txt needs: the full interactive std::cin picker (below, via
  // SelectDevices()) only runs when devices.txt doesn't already exist
  // AND a real terminal is attached, which meant there was previously no
  // way to just look at what's detected without also either deleting a
  // working devices.txt or being dropped into the picker. See
  // codefix.md Round 9 #2.
  if (argc > 1 && strcmp(argv[1], "list-devices") == 0) {
    AudioTransformer::DumpDeviceInfo();
    return 0;
  }

  bool in_debug_mode = argc > 1 && strcmp(argv[1], "debug") == 0;

  // NOTE: hardware_service.py (GPIO footswitches + OLED) is intentionally
  // *not* started from here. It is owned by its own systemd unit
  // (setup/oleander-hardware.service) so that it and the audio/web server
  // can be restarted independently, and so it is only ever running as a
  // single instance -- starting it a second time here used to race with
  // the copy systemd already launches, with both processes fighting over
  // the same GPIO pins and I2C display. If you are running the server
  // manually (not via systemd), start hardware_service.py yourself in a
  // separate terminal.

  crow::SimpleApp app;
  PedalBoard pedal_board;
  PresetStore preset_store("presets.json");
  ChainLibrary chain_library("chain_library.json");
  SwitchStates switch_states;

  // Resume with whatever was last dialed in (including a mid-edit, not-yet
  // -saved-as-a-preset state) rather than coming back to an empty board
  // after every restart.
  auto initial_state = preset_store.GetCurrent();
  if (!initial_state.pedals.empty()) {
    pedal_board.LoadSnapshot(initial_state.pedals);
  }

  ActivePedalHandler active_pedal_handler(&pedal_board);
  CROW_ROUTE(app, "/active_pedals")(active_pedal_handler);

  UpdatesHandler updates_handler;
  CROW_ROUTE(app, "/updates")
      .websocket()
      .onopen([&](crow::websocket::connection& conn) {
        updates_handler.RegisterConnection(&conn);
      })
      .onclose(
          [&](crow::websocket::connection& conn, const std::string& reason) {
            updates_handler.RemoveConnection(&conn);
          });

  ChangeNotifier notifier(&pedal_board, &preset_store, &updates_handler);

  AddPedalHandler add_pedal_handler(&pedal_board, notifier);
  CROW_ROUTE(app, "/add_pedal/<string>")(add_pedal_handler);

  RemovePedalHandler remove_pedal_handler(&pedal_board, notifier);
  CROW_ROUTE(app, "/remove_pedal/<int>")(remove_pedal_handler);

  // `<int>` here is a pedal's stable id (PedalInfo::id), not its position
  // in the chain -- see web/pedal_board.h.
  PushButtonHandler push_button_handler(&pedal_board, notifier);
  CROW_ROUTE(app, "/push_button/<int>")(push_button_handler);

  AdjustKnobHandler adjust_knob_handler(&pedal_board, notifier);
  CROW_ROUTE(app, "/adjust_knob/<int>")(adjust_knob_handler);

  AvailablePedalHandler available_pedal_handler;
  CROW_ROUTE(app, "/available_pedals")(available_pedal_handler);

  // Presets: 5 named, saveable snapshots of the whole board. `<int>` here
  // is a preset slot (0-4), unrelated to the pedal ids used above. This is
  // what the 5 physical footswitches call.
  PresetListHandler preset_list_handler(&preset_store);
  CROW_ROUTE(app, "/presets")(preset_list_handler);

  LoadPresetHandler load_preset_handler(&pedal_board, &preset_store, notifier);
  CROW_ROUTE(app, "/preset/<int>")(load_preset_handler);

  SavePresetHandler save_preset_handler(&pedal_board, &preset_store,
                                         &chain_library, notifier);
  CROW_ROUTE(app, "/preset/<int>/save")
      .methods(crow::HTTPMethod::POST)(save_preset_handler);

  RenamePresetHandler rename_preset_handler(&preset_store, notifier);
  CROW_ROUTE(app, "/preset/<int>/name")
      .methods(crow::HTTPMethod::POST)(rename_preset_handler);

  // Chain library: unbounded, name-keyed collection of saved chains,
  // independent of the 5 fixed preset slots above. Populated automatically
  // by SavePresetHandler (every "Save here" upserts into it) -- there is no
  // direct save endpoint here. See docs/ROADMAP.md item 2.
  ChainLibraryListHandler chain_library_list_handler(&chain_library);
  CROW_ROUTE(app, "/chain_library")(chain_library_list_handler);

  ChainLibraryDeleteHandler chain_library_delete_handler(&chain_library,
                                                          notifier);
  CROW_ROUTE(app, "/chain_library/<string>/delete")
      .methods(crow::HTTPMethod::POST)(chain_library_delete_handler);

  ChainLibraryAssignHandler chain_library_assign_handler(
      &preset_store, &chain_library, notifier);
  CROW_ROUTE(app, "/chain_library/<string>/assign/<int>")
      .methods(crow::HTTPMethod::POST)(chain_library_assign_handler);

  // Live latch state of the 5 physical footswitches (separate from, and in
  // addition to, the preset each one recalls) -- backs the on/off
  // indicator dot on each preset tile in the web UI. See codefix.md
  // Round 8 #1.
  SwitchListHandler switch_list_handler(&switch_states);
  CROW_ROUTE(app, "/switches")(switch_list_handler);

  SwitchStateHandler switch_state_handler(&switch_states, &updates_handler);
  CROW_ROUTE(app, "/switch/<int>/state")
      .methods(crow::HTTPMethod::POST)(switch_state_handler);

  // "web/static" here is relative to the process's working directory, which
  // is the repo root under systemd (setup/oleander.service sets
  // WorkingDirectory=__OLEANDER_DIR__, the checkout root, not web/) -- these
  // used to say just "static", which only resolved correctly when the
  // server happened to be launched with web/ itself as the cwd (e.g. a
  // manual `cd web && ../bin/server debug`). Under the real systemd
  // deployment that meant every request, including "/" for index.html
  // itself, 404'd looking for a top-level static/ directory that doesn't
  // exist -- see codefix.md Round 7 #1.
  CROW_ROUTE(app, "/")
  ([]() {
    StaticFileHandler static_file_handler(/* directory = */ "web/static");
    return static_file_handler("index.html");
  });

  StaticFileHandler static_file_handler(/* directory = */ "web/static");
  CROW_ROUTE(app, "/<string>")(static_file_handler);

  FrameBuffer frame_buffer(/* max_size= */ 44100);
  Visualizer v(&frame_buffer, /* fps= */ 30);

  Playback pb(/* filename= */ "../recording");
  auto transform = [in_debug_mode, &pedal_board, &pb,
                    &frame_buffer](SignalType input) {
    auto out = pedal_board.Transform(in_debug_mode ? pb.next() : input);
    frame_buffer.Add(out);
    return out;
  };

  auto selections = SelectDevices();
  AudioTransformer at(transform, selections.input_device.device_index,
                      selections.output_device.device_index);
  at.Start();

  if (!in_debug_mode) {
    app.loglevel(crow::LogLevel::WARNING);
  }

  std::cout << "[MAIN] Audio engine started. Web server on port "
            << (in_debug_mode ? 8080 : 80) << std::endl;

  std::thread app_thread([&]() { app.port(in_debug_mode ? 8080 : 80).run(); });
  v.BlockingStart();
  app_thread.join();
}
