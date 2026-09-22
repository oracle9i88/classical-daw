#include "coremidi_host.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <utility>

namespace daw {
namespace {

void setError(std::string* error, const char* operation, OSStatus status) {
  if (error != nullptr) {
    *error = std::string(operation) + " failed with OSStatus " + std::to_string(status);
  }
}

CFStringRef makeString(const std::string& value) {
  return CFStringCreateWithCString(kCFAllocatorDefault, value.c_str(), kCFStringEncodingUTF8);
}

std::string endpointName(MIDIEndpointRef endpoint) {
  CFStringRef value = nullptr;
  if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &value) != noErr || value == nullptr) {
    return "Unnamed MIDI endpoint";
  }

  char buffer[512]{};
  const bool converted = CFStringGetCString(value, buffer, static_cast<CFIndex>(sizeof(buffer)),
                                            kCFStringEncodingUTF8);
  CFRelease(value);
  return converted ? std::string(buffer) : std::string("Unnamed MIDI endpoint");
}

bool endpointId(MIDIEndpointRef endpoint, std::int32_t* id) {
  if (id == nullptr) return false;
  SInt32 value = 0;
  if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &value) != noErr) {
    return false;
  }
  *id = static_cast<std::int32_t>(value);
  return true;
}

}  // namespace

struct CoreMidiHost::Impl {
  struct ConnectedSource {
    std::int32_t id = 0;
    MIDIEndpointRef endpoint = 0;
  };

  explicit Impl(CoreMidiHostConfig value) : config(std::move(value)) {}

  static void readProc(const MIDIPacketList* packet_list, void* read_ref_con,
                       void* /*source_connection_ref_con*/) {
    auto* impl = static_cast<Impl*>(read_ref_con);
    if (impl == nullptr || !impl->running.load(std::memory_order_acquire) || packet_list == nullptr) {
      return;
    }
    // CoreMIDI invokes this on a high-priority receive thread. Keep it bounded
    // and allocation-free. A future MIDI event queue can consume this count at
    // a control-thread boundary; it must never call the audio renderer here.
    impl->received_packets.fetch_add(static_cast<std::uint64_t>(packet_list->numPackets),
                                     std::memory_order_relaxed);
  }

  CoreMidiHostConfig config;
  MIDIClientRef client = 0;
  MIDIPortRef input_port = 0;
  MIDIPortRef output_port = 0;
  std::vector<ConnectedSource> connected_sources;
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> received_packets{0};
};

CoreMidiHost::CoreMidiHost(CoreMidiHostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

CoreMidiHost::~CoreMidiHost() { stop(); }

CoreMidiHost::CoreMidiHost(CoreMidiHost&& other) noexcept = default;

CoreMidiHost& CoreMidiHost::operator=(CoreMidiHost&& other) noexcept {
  if (this != &other) {
    stop();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool CoreMidiHost::start(std::string* error) {
  if (impl_ == nullptr) {
    if (error != nullptr) *error = "CoreMIDI host has no implementation state";
    return false;
  }
  if (impl_->running.load(std::memory_order_acquire)) return true;

  CFStringRef client_name = makeString(impl_->config.client_name);
  CFStringRef input_name = makeString(impl_->config.input_port_name);
  CFStringRef output_name = makeString(impl_->config.output_port_name);
  if (client_name == nullptr || input_name == nullptr || output_name == nullptr) {
    if (client_name != nullptr) CFRelease(client_name);
    if (input_name != nullptr) CFRelease(input_name);
    if (output_name != nullptr) CFRelease(output_name);
    if (error != nullptr) *error = "unable to allocate CoreMIDI names";
    return false;
  }

  OSStatus status = MIDIClientCreate(client_name, nullptr, nullptr, &impl_->client);
  CFRelease(client_name);
  if (status != noErr) {
    setError(error, "MIDIClientCreate", status);
    impl_->client = 0;
    CFRelease(input_name);
    CFRelease(output_name);
    return false;
  }

  status = MIDIInputPortCreate(impl_->client, input_name, &Impl::readProc, impl_.get(),
                               &impl_->input_port);
  CFRelease(input_name);
  if (status != noErr) {
    setError(error, "MIDIInputPortCreate", status);
    CFRelease(output_name);
    stop();
    return false;
  }

  status = MIDIOutputPortCreate(impl_->client, output_name, &impl_->output_port);
  CFRelease(output_name);
  if (status != noErr) {
    setError(error, "MIDIOutputPortCreate", status);
    stop();
    return false;
  }

  impl_->running.store(true, std::memory_order_release);
  return true;
}

void CoreMidiHost::stop() noexcept {
  if (impl_ == nullptr) return;
  impl_->running.store(false, std::memory_order_release);

  if (impl_->input_port != 0) {
    for (const auto& source : impl_->connected_sources) {
      (void)MIDIPortDisconnectSource(impl_->input_port, source.endpoint);
    }
    impl_->connected_sources.clear();
    (void)MIDIPortDispose(impl_->input_port);
    impl_->input_port = 0;
  }
  if (impl_->output_port != 0) {
    (void)MIDIPortDispose(impl_->output_port);
    impl_->output_port = 0;
  }
  if (impl_->client != 0) {
    (void)MIDIClientDispose(impl_->client);
    impl_->client = 0;
  }
}

bool CoreMidiHost::running() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

std::vector<CoreMidiEndpointInfo> CoreMidiHost::enumerateSources(std::string* error) const {
  std::vector<CoreMidiEndpointInfo> endpoints;
  const ItemCount count = MIDIGetNumberOfSources();
  endpoints.reserve(static_cast<std::size_t>(count));
  for (ItemCount index = 0; index < count; ++index) {
    const MIDIEndpointRef endpoint = MIDIGetSource(index);
    std::int32_t id = 0;
    if (endpoint == 0 || !endpointId(endpoint, &id)) {
      if (error != nullptr && error->empty()) *error = "unable to read a CoreMIDI source ID";
      continue;
    }
    endpoints.push_back(CoreMidiEndpointInfo{id, endpointName(endpoint)});
  }
  return endpoints;
}

std::vector<CoreMidiEndpointInfo> CoreMidiHost::enumerateDestinations(std::string* error) const {
  std::vector<CoreMidiEndpointInfo> endpoints;
  const ItemCount count = MIDIGetNumberOfDestinations();
  endpoints.reserve(static_cast<std::size_t>(count));
  for (ItemCount index = 0; index < count; ++index) {
    const MIDIEndpointRef endpoint = MIDIGetDestination(index);
    std::int32_t id = 0;
    if (endpoint == 0 || !endpointId(endpoint, &id)) {
      if (error != nullptr && error->empty()) *error = "unable to read a CoreMIDI destination ID";
      continue;
    }
    endpoints.push_back(CoreMidiEndpointInfo{id, endpointName(endpoint)});
  }
  return endpoints;
}

bool CoreMidiHost::connectSource(std::int32_t endpoint_id, std::string* error) {
  if (!running() || impl_->input_port == 0) {
    if (error != nullptr) *error = "CoreMIDI host is not running";
    return false;
  }
  const ItemCount count = MIDIGetNumberOfSources();
  for (ItemCount index = 0; index < count; ++index) {
    const MIDIEndpointRef endpoint = MIDIGetSource(index);
    std::int32_t id = 0;
    if (endpoint == 0 || !endpointId(endpoint, &id) || id != endpoint_id) continue;
    const auto already_connected = std::find_if(
        impl_->connected_sources.begin(), impl_->connected_sources.end(),
        [endpoint_id](const Impl::ConnectedSource& source) { return source.id == endpoint_id; });
    if (already_connected != impl_->connected_sources.end()) return true;

    const OSStatus status = MIDIPortConnectSource(impl_->input_port, endpoint, nullptr);
    if (status != noErr) {
      setError(error, "MIDIPortConnectSource", status);
      return false;
    }
    try {
      impl_->connected_sources.push_back(Impl::ConnectedSource{endpoint_id, endpoint});
    } catch (...) {
      // The CoreMIDI connection is already live. Roll it back before
      // converting allocation failure into the adapter's bool error contract.
      (void)MIDIPortDisconnectSource(impl_->input_port, endpoint);
      if (error != nullptr) *error = "unable to retain CoreMIDI source connection";
      return false;
    }
    return true;
  }
  if (error != nullptr) *error = "CoreMIDI source endpoint not found";
  return false;
}

bool CoreMidiHost::disconnectSource(std::int32_t endpoint_id, std::string* error) {
  if (!running() || impl_->input_port == 0) {
    if (error != nullptr) *error = "CoreMIDI host is not running";
    return false;
  }
  const auto connected = std::find_if(
      impl_->connected_sources.begin(), impl_->connected_sources.end(),
      [endpoint_id](const Impl::ConnectedSource& source) { return source.id == endpoint_id; });
  if (connected == impl_->connected_sources.end()) {
    if (error != nullptr) *error = "CoreMIDI source endpoint is not connected";
    return false;
  }
  const OSStatus status = MIDIPortDisconnectSource(impl_->input_port, connected->endpoint);
  if (status != noErr) {
    setError(error, "MIDIPortDisconnectSource", status);
    return false;
  }
  impl_->connected_sources.erase(connected);
  return true;
}

std::uint64_t CoreMidiHost::receivedPacketCount() const noexcept {
  return impl_ == nullptr ? 0 : impl_->received_packets.load(std::memory_order_relaxed);
}

}  // namespace daw
