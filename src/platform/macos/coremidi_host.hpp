#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

// A stable, device-agnostic view of a CoreMIDI endpoint. The numeric ID is
// the endpoint's persistent MIDIUniqueID; callers do not need to retain a
// CoreMIDI object handle across setup changes.
struct CoreMidiEndpointInfo {
  std::int32_t id = 0;
  std::string name;
};

struct CoreMidiHostConfig {
  std::string client_name = "Classical DAW";
  std::string input_port_name = "Classical DAW Input";
  std::string output_port_name = "Classical DAW Output";
};

// Minimal CoreMIDI lifecycle adapter. It owns the client and ports and can
// enumerate or connect physical sources without exposing CoreMIDI handles to
// the rest of the engine. Incoming packets are deliberately not forwarded to
// the audio callback: the CoreMIDI receive thread only increments a counter.
class CoreMidiHost {
 public:
  explicit CoreMidiHost(CoreMidiHostConfig config = {});
  ~CoreMidiHost();

  CoreMidiHost(const CoreMidiHost&) = delete;
  CoreMidiHost& operator=(const CoreMidiHost&) = delete;
  CoreMidiHost(CoreMidiHost&&) noexcept;
  CoreMidiHost& operator=(CoreMidiHost&&) noexcept;

  bool start(std::string* error = nullptr);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;

  [[nodiscard]] std::vector<CoreMidiEndpointInfo> enumerateSources(
      std::string* error = nullptr) const;
  [[nodiscard]] std::vector<CoreMidiEndpointInfo> enumerateDestinations(
      std::string* error = nullptr) const;

  // Connections are intentionally explicit. This prevents a newly-plugged
  // device from silently entering a live performance and makes disconnects
  // deterministic during stop().
  bool connectSource(std::int32_t endpoint_id, std::string* error = nullptr);
  bool disconnectSource(std::int32_t endpoint_id, std::string* error = nullptr);

  // Number of CoreMIDI packets observed by the input port. Packet data is not
  // retained or sent to the realtime audio renderer by this adapter.
  [[nodiscard]] std::uint64_t receivedPacketCount() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace daw
