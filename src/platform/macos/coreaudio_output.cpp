#include "coreaudio_output.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cmath>
#include <cstring>

namespace daw {
namespace {

bool checkStatus(OSStatus status, const char* operation, std::string* error) {
  if (status == noErr) return true;
  if (error != nullptr) *error = std::string(operation) + " failed with OSStatus " + std::to_string(status);
  return false;
}

}  // namespace

CoreAudioOutput::CoreAudioOutput(CoreAudioOutputConfig config) : config_(config) {
  scheduler_.setInstrumentRenderer(&synth_);
}

CoreAudioOutput::~CoreAudioOutput() { stop(); }

bool CoreAudioOutput::start(std::string* error) {
  if (running()) return true;
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
  if (!checkStatus(AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_SetInputCallback,
                                        kAudioUnitScope_Global, 0, &callback, sizeof(callback)),
                   "input callback", error)) {
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
    output->scheduler_.recordXrun();
    return noErr;
  }
  output->scheduler_.processBlock(frame_count);
  const std::size_t required_bytes = static_cast<std::size_t>(frame_count) * output->config_.channels * sizeof(float);
  if (buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mData != nullptr &&
      buffers->mBuffers[0].mDataByteSize >= required_bytes) {
    auto& buffer = buffers->mBuffers[0];
    output->synth_.render(static_cast<float*>(buffer.mData), frame_count, output->config_.channels, output->config_.sample_rate);
  } else {
    output->scheduler_.recordXrun();
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
      AudioBuffer& buffer = buffers->mBuffers[index];
      if (buffer.mData != nullptr && buffer.mDataByteSize > 0) std::memset(buffer.mData, 0, buffer.mDataByteSize);
    }
  }
  return noErr;
}

}  // namespace daw
