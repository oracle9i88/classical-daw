#include "coreaudio_output.hpp"
#include "daw/output_blocks.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace daw {
namespace {
// Stack-only scope timer includes validation, every engine subdivision, source
// rendering and host bookkeeping, including all early-return/error paths. The
// end timestamp precedes the small accounting operation itself.
class CallbackTimer {
 public:
  CallbackTimer(CallbackTimingStats& stats, std::chrono::steady_clock::time_point start,
                std::uint32_t frames, double rate) noexcept
      : stats_(stats), start_(start), frames_(frames), rate_(rate) {}
  ~CallbackTimer() {
    const auto end = std::chrono::steady_clock::now();
    stats_.record(frames_, rate_, std::chrono::duration<double>(end - start_).count());
  }
 private:
  CallbackTimingStats& stats_;
  std::chrono::steady_clock::time_point start_;
  std::uint32_t frames_;
  double rate_;
};

std::uint64_t milliseconds() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool checkStatus(OSStatus status, const char* operation, std::string* error) {
  if (status == noErr) return true;
  if (error != nullptr) *error = std::string(operation) + " failed with OSStatus " + std::to_string(status);
  return false;
}

bool readDefaultOutputDevice(AudioDeviceID* device, std::string* error) {
  if (device == nullptr) {
    if (error != nullptr) *error = "default CoreAudio device output is null";
    return false;
  }
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(*device);
  if (!checkStatus(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, device),
                     "default output device", error)) return false;
  if (*device == kAudioObjectUnknown) {
    if (error) *error = "no default CoreAudio output device is available";
    return false;
  }
  return true;
}

bool hasOutputChannels(AudioDeviceID device) {
  AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,
                                    kAudioDevicePropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList)) {
    return false;
  }
  std::vector<std::uint8_t> storage(size);
  auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) return false;
  UInt32 channels = 0;
  for (UInt32 index = 0; index < list->mNumberBuffers; ++index) channels += list->mBuffers[index].mNumberChannels;
  return channels != 0;
}

std::string deviceName(AudioDeviceID device) {
  AudioObjectPropertyAddress address{kAudioObjectPropertyName,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
  CFStringRef value = nullptr;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr) {
    return "Unnamed CoreAudio output";
  }
  char buffer[512]{};
  const bool converted = CFStringGetCString(value, buffer, static_cast<CFIndex>(sizeof(buffer)), kCFStringEncodingUTF8);
  CFRelease(value);
  return converted ? std::string(buffer) : std::string("Unnamed CoreAudio output");
}

}  // namespace

CoreAudioOutput::CoreAudioOutput(CoreAudioOutputConfig config) : config_(config) {
  scheduler_.setInstrumentRenderer(&synth_);
}

CoreAudioOutput::~CoreAudioOutput() { stop(); }

bool CoreAudioOutput::start(std::string* error) {
  if (running()) return true;
  callback_timing_ = {};
  if (source_ && !source_->acceptsFormat(config_.sample_rate, config_.channels)) {
    if (error) *error = "audio source does not support this output format";
    return false;
  }
  if (!std::isfinite(config_.sample_rate) || config_.sample_rate <= 0.0 || config_.block_size == 0 ||
      config_.block_size > 65536 ||
      config_.channels == 0 || config_.channels > 8) {
    if (error != nullptr) *error = "invalid CoreAudio output configuration";
    return false;
  }

  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_DefaultOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr) {
    if (error != nullptr) *error = "default CoreAudio output component not found";
    return false;
  }
  if (!checkStatus(AudioComponentInstanceNew(component, &audio_unit_), "AudioComponentInstanceNew", error)) {
    audio_unit_ = nullptr;
    return false;
  }

  AudioDeviceID device = static_cast<AudioDeviceID>(config_.device_id);
  if (device == kAudioObjectUnknown && !readDefaultOutputDevice(&device, error)) {
    stop();
    return false;
  }
  if (!hasOutputChannels(device)) {
    if (error != nullptr) *error = "selected CoreAudio device has no output channels";
    stop();
    return false;
  }
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
                                        kAudioUnitScope_Global, 0, &device, sizeof(device)),
                   "current output device", error)) {
    stop();
    return false;
  }
  current_device_id_ = static_cast<std::uint32_t>(device);

  AudioStreamBasicDescription format{};
  format.mSampleRate = config_.sample_rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  format.mBytesPerPacket = sizeof(float) * config_.channels;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(float) * config_.channels;
  format.mChannelsPerFrame = config_.channels;
  format.mBitsPerChannel = 32;
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                        &format, sizeof(format)),
                   "stream format", error)) {
    stop();
    return false;
  }

  AudioObjectPropertyAddress device_property{kAudioDevicePropertyBufferFrameSize,
      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 device_frames = 0, size = sizeof(device_frames);
  if (!checkStatus(AudioObjectGetPropertyData(device, &device_property, 0, nullptr, &size, &device_frames),
                   "device buffer frames", error)) { stop(); return false; }
  device_property.mSelector = kAudioDevicePropertyNominalSampleRate;
  Float64 device_rate = 0; size = sizeof(device_rate);
  if (!checkStatus(AudioObjectGetPropertyData(device, &device_property, 0, nullptr, &size, &device_rate),
                   "device sample rate", error)) { stop(); return false; }
  UInt32 capacity = 0;
  try { capacity = outputSliceCapacity(device_frames, device_rate, config_.sample_rate, config_.block_size); }
  catch (const std::exception& e) { if (error) *error = e.what(); stop(); return false; }
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global, 0, &capacity, sizeof(capacity)), "maximum frames per slice", error)) {
    stop(); return false;
  }

  AURenderCallbackStruct callback{};
  callback.inputProc = reinterpret_cast<AURenderCallback>(renderCallback);
  callback.inputProcRefCon = this;
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_SetRenderCallback,
                                        kAudioUnitScope_Input, 0, &callback, sizeof(callback)),
                   "render callback", error)) {
    stop();
    return false;
  }
  if (!synth_.prepare(InstrumentRenderConfig{config_.sample_rate, config_.block_size, config_.channels})) {
    if (error != nullptr) *error = "diagnostic instrument preparation failed";
    stop();
    return false;
  }
  if (!checkStatus(AudioUnitInitialize(audio_unit_), "AudioUnitInitialize", error)) {
    stop();
    return false;
  }
  size = sizeof(maximum_callback_frames_);
  if (!checkStatus(AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global, 0, &maximum_callback_frames_, &size), "prepared maximum frames", error)) {
    stop(); return false;
  }
  if (!maximum_callback_frames_ || maximum_callback_frames_ > 65536) {
    if (error) *error = "prepared AU callback capacity is outside supported bounds";
    stop(); return false;
  }
  rendered_frames_.store(0, std::memory_order_relaxed);
  callback_errors_.store(0, std::memory_order_relaxed);
  if (!checkStatus(AudioOutputUnitStart(audio_unit_), "AudioOutputUnitStart", error)) {
    stop();
    return false;
  }
  running_.store(true, std::memory_order_release);
  // A successful start call alone does not prove the device is pulling audio.
  // Bound this control-thread wait; callbacks never sleep or query properties.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!renderedFrames() && !xrunCount() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!renderedFrames() || xrunCount()) {
    if (error) *error = "output failed callback readiness: " + diagnostics();
    stop(); return false;
  }
  auto initial = healthSnapshot();
  // Compare against the device/format actually used to prepare this AU, not a
  // possibly changed route observed after startup. checkHealth takes a fresh
  // snapshot and rejects a change during the readiness wait as well.
  initial.device = current_device_id_;
  initial.sample_rate = device_rate; initial.buffer_frames = device_frames;
  health_.arm(initial, config_.device_id == 0, milliseconds());
  if (!checkHealth(error)) { stop(); return false; }
  if (error) error->clear();
  return true;
}

OutputHealthSnapshot CoreAudioOutput::healthSnapshot() const noexcept {
  OutputHealthSnapshot s;
  s.device = current_device_id_; s.rendered_frames = renderedFrames(); s.callback_errors = xrunCount();
  AudioDeviceID active = 0; UInt32 size = sizeof(active);
  if (!audio_unit_ || AudioUnitGetProperty(audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
      kAudioUnitScope_Global, 0, &active, &size) != noErr) return s;
  s.device = active;
  AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive,
      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 alive = 0; size = sizeof(alive);
  if (AudioObjectGetPropertyData(current_device_id_, &address, 0, nullptr, &size, &alive) != noErr) return s;
  s.alive = alive != 0;
  address.mSelector = kAudioDevicePropertyBufferFrameSize; size = sizeof(s.buffer_frames);
  if (AudioObjectGetPropertyData(current_device_id_, &address, 0, nullptr, &size, &s.buffer_frames) != noErr) return s;
  address.mSelector = kAudioDevicePropertyNominalSampleRate; size = sizeof(s.sample_rate);
  if (AudioObjectGetPropertyData(current_device_id_, &address, 0, nullptr, &size, &s.sample_rate) != noErr) return s;
  if (config_.device_id == 0) {
    address.mSelector = kAudioHardwarePropertyDefaultOutputDevice; size = sizeof(s.default_device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &s.default_device) != noErr) return s;
  }
  s.readable = true;
  return s;
}

bool CoreAudioOutput::checkHealth(std::string* error) {
  const auto fault = running() ? health_.observe(healthSnapshot(), milliseconds()) : OutputFault::Stopped;
  if (error) *error = fault == OutputFault::None ? "" : outputFaultText(fault);
  return fault == OutputFault::None;
}

void CoreAudioOutput::stop() noexcept {
  running_.store(false, std::memory_order_release);
  if (audio_unit_ != nullptr) {
    (void)AudioOutputUnitStop(audio_unit_);
    (void)AudioUnitUninitialize(audio_unit_);
    (void)AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
  }
  synth_.reset();
  current_device_id_ = 0;
}

std::vector<CoreAudioOutputDeviceInfo> CoreAudioOutput::enumerateOutputDevices(std::string* error) const {
  std::vector<CoreAudioOutputDeviceInfo> devices;
  AudioDeviceID default_device = kAudioObjectUnknown;
  // A missing default must not prevent choosing another available output.
  (void)readDefaultOutputDevice(&default_device, nullptr);
  if (error) error->clear();

  AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (!checkStatus(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size),
                   "CoreAudio device list size", error)) {
    return devices;
  }
  if (size == 0 || size % sizeof(AudioDeviceID) != 0) {
    if (error != nullptr) *error = "CoreAudio device list has an invalid size";
    return devices;
  }
  std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
  if (!checkStatus(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()),
                   "CoreAudio device list", error)) {
    return devices;
  }
  devices.reserve(ids.size());
  for (const AudioDeviceID id : ids) {
    if (!hasOutputChannels(id)) continue;
    devices.push_back(CoreAudioOutputDeviceInfo{static_cast<std::uint32_t>(id), deviceName(id), id == default_device});
  }
  return devices;
}

bool CoreAudioOutput::setOutputDevice(std::uint32_t device_id, std::string* error) {
  if (running()) {
    if (error != nullptr) *error = "cannot change CoreAudio device while output is running";
    return false;
  }
  if (device_id != 0) {
    bool found = false;
    for (const CoreAudioOutputDeviceInfo& device : enumerateOutputDevices(error)) {
      if (device.id == device_id) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (error != nullptr && error->empty()) *error = "CoreAudio output device not found";
      return false;
    }
  }
  config_.device_id = device_id;
  current_device_id_ = 0;
  if (error != nullptr) error->clear();
  return true;
}

bool CoreAudioOutput::setAudioSource(AudioOutputSource* source, std::string* error) {
  if (running()) {
    if (error) *error = "cannot replace an audio source while output is running";
    return false;
  }
  if (source && !source->acceptsFormat(config_.sample_rate, config_.channels)) {
    if (error) *error = "audio source does not support this output format";
    return false;
  }
  source_ = source;
  if (error) error->clear();
  return true;
}

std::string CoreAudioOutput::diagnostics() const {
  std::ostringstream out;
  out << "device=" << current_device_id_ << " client_rate=" << config_.sample_rate
      << " processed_frames=" << renderedFrames() << " callback_errors=" << xrunCount();
  if (audio_unit_) {
    OSStatus last_error = 0;
    UInt32 size = sizeof(last_error);
    if (AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_LastRenderError,
        kAudioUnitScope_Global, 0, &last_error, &size) == noErr) out << " last_render_error=" << last_error;
    UInt32 maximum = 0; size = sizeof(maximum);
    if (AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_MaximumFramesPerSlice,
        kAudioUnitScope_Global, 0, &maximum, &size) == noErr) out << " au_maximum_frames=" << maximum;
  }
  if (current_device_id_) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 frames = 0, size = sizeof(frames);
    if (AudioObjectGetPropertyData(current_device_id_, &address, 0, nullptr, &size, &frames) == noErr)
      out << " device_buffer_frames=" << frames;
    address.mSelector = kAudioDevicePropertyNominalSampleRate;
    Float64 rate = 0; size = sizeof(rate);
    if (AudioObjectGetPropertyData(current_device_id_, &address, 0, nullptr, &size, &rate) == noErr)
      out << " device_rate=" << rate;
  }
  return out.str();
}

CallbackTimingStats CoreAudioOutput::callbackTimingAfterStop() const {
  if (audio_unit_ != nullptr) {
    throw std::logic_error("callback timing is only readable after CoreAudioOutput::stop returns");
  }
  return callback_timing_;
}

OSStatus CoreAudioOutput::renderCallback(void* reference,
                                         AudioUnitRenderActionFlags* /*action_flags*/,
                                         const AudioTimeStamp* /*timestamp*/,
                                         UInt32 /*bus_number*/,
                                         UInt32 frame_count,
                                         AudioBufferList* buffers) noexcept {
  const auto callback_start = std::chrono::steady_clock::now();
  auto* output = static_cast<CoreAudioOutput*>(reference);
  if (output == nullptr) return noErr;
  CallbackTimer timer(output->callback_timing_, callback_start, frame_count, output->config_.sample_rate);
  if (buffers == nullptr) {
    output->callback_errors_.fetch_add(1, std::memory_order_relaxed);
    output->scheduler_.recordXrun();
    return noErr;
  }
  const std::size_t required_bytes = static_cast<std::size_t>(frame_count) * output->config_.channels * sizeof(float);
  if (buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mData != nullptr &&
      frame_count <= output->maximum_callback_frames_ &&
      buffers->mBuffers[0].mNumberChannels == output->config_.channels &&
      buffers->mBuffers[0].mDataByteSize >= required_bytes) {
    auto& buffer = buffers->mBuffers[0];
    renderOutputBlocks(static_cast<float*>(buffer.mData), frame_count, output->config_.channels,
        output->config_.block_size, [output](float* samples, std::uint32_t count) noexcept {
          if (output->source_) output->source_->render(samples, count);
          else {
            output->scheduler_.processBlock(count);
            output->synth_.render(samples, count, output->config_.channels, output->config_.sample_rate);
          }
        });
    output->rendered_frames_.fetch_add(frame_count, std::memory_order_relaxed);
  } else {
    output->callback_errors_.fetch_add(1, std::memory_order_relaxed);
    output->scheduler_.recordXrun();
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
      AudioBuffer& buffer = buffers->mBuffers[index];
      if (buffer.mData != nullptr && buffer.mDataByteSize > 0) std::memset(buffer.mData, 0, buffer.mDataByteSize);
    }
  }
  return noErr;
}

}  // namespace daw
