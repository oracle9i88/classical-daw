#include "coreaudio_output.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace daw {
namespace {

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
  return checkStatus(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, device),
                     "default output device", error) && *device != kAudioObjectUnknown;
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
  if (source_ && !source_->acceptsFormat(config_.sample_rate, config_.channels)) {
    if (error) *error = "audio source does not support this output format";
    return false;
  }
  if (!std::isfinite(config_.sample_rate) || config_.sample_rate <= 0.0 || config_.block_size == 0 ||
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

  const UInt32 block_size = config_.block_size;
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                                        kAudioUnitScope_Global, 0, &block_size, sizeof(block_size)),
                   "maximum frames per slice", error)) {
    stop();
    return false;
  }

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
  if (!checkStatus(AudioOutputUnitStart(audio_unit_), "AudioOutputUnitStart", error)) {
    stop();
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
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
  if (!readDefaultOutputDevice(&default_device, error)) return devices;

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

OSStatus CoreAudioOutput::renderCallback(void* reference,
                                         AudioUnitRenderActionFlags* /*action_flags*/,
                                         const AudioTimeStamp* /*timestamp*/,
                                         UInt32 /*bus_number*/,
                                         UInt32 frame_count,
                                         AudioBufferList* buffers) noexcept {
  auto* output = static_cast<CoreAudioOutput*>(reference);
  if (output == nullptr) return noErr;
  if (buffers == nullptr) {
    output->callback_errors_.fetch_add(1, std::memory_order_relaxed);
    output->scheduler_.recordXrun();
    return noErr;
  }
  const std::size_t required_bytes = static_cast<std::size_t>(frame_count) * output->config_.channels * sizeof(float);
  if (buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mData != nullptr &&
      frame_count <= output->config_.block_size &&
      buffers->mBuffers[0].mNumberChannels == output->config_.channels &&
      buffers->mBuffers[0].mDataByteSize >= required_bytes) {
    auto& buffer = buffers->mBuffers[0];
    if (output->source_) output->source_->render(static_cast<float*>(buffer.mData), frame_count);
    else {
      output->scheduler_.processBlock(frame_count);
      output->synth_.render(static_cast<float*>(buffer.mData), frame_count, output->config_.channels, output->config_.sample_rate);
    }
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
