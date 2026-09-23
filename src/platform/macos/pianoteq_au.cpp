#include "pianoteq_au.hpp"
#include "daw/midi_sequence.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace daw {
namespace {
constexpr OSType kSubtype = 'Pt9q';
constexpr OSType kManufacturer = 'Mdrt';
constexpr std::size_t kStateLimit = 16U * 1024U * 1024U;
constexpr UInt32 kBlock = 256;

void check(OSStatus status, const char* operation) {
  if (status != noErr) throw std::runtime_error(std::string(operation) + ": OSStatus " + std::to_string(status));
}

template <typename T> struct CFHandle {
  T value = nullptr;
  ~CFHandle() { if (value) CFRelease(value); }
  CFHandle() = default;
  CFHandle(const CFHandle&) = delete;
  CFHandle& operator=(const CFHandle&) = delete;
};

std::string utf8(CFStringRef string) {
  if (!string) throw std::runtime_error("AU returned a missing preset name");
  const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8);
  if (size < 0 || size > 1048576) throw std::runtime_error("AU preset name is too large");
  std::vector<char> bytes(static_cast<std::size_t>(size) + 1);
  if (!CFStringGetCString(string, bytes.data(), static_cast<CFIndex>(bytes.size()), kCFStringEncodingUTF8)) {
    throw std::runtime_error("AU preset name is not UTF-8");
  }
  return bytes.data();
}

void requireIdentity(CFPropertyListRef value) {
  if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID()) throw std::runtime_error("AU state must be a dictionary");
  const auto dictionary = static_cast<CFDictionaryRef>(value);
  auto numberMatches = [&](CFStringRef key, std::int64_t expected) {
    auto number = CFDictionaryGetValue(dictionary, key);
    std::int64_t actual = 0;
    return number && CFGetTypeID(number) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(number), kCFNumberSInt64Type, &actual) && actual == expected;
  };
  if (!numberMatches(CFSTR(kAUPresetTypeKey), kAudioUnitType_MusicDevice) ||
      !numberMatches(CFSTR(kAUPresetSubtypeKey), kSubtype) ||
      !numberMatches(CFSTR(kAUPresetManufacturerKey), kManufacturer)) {
    throw std::runtime_error("AU state does not belong to Pianoteq 9 (aumu/Pt9q/Mdrt)");
  }
}
}  // namespace

struct PianoteqAU::Impl {
  AudioComponent component = nullptr;
  AudioUnit unit = nullptr;
  bool initialized = false;
  bool consumed = false;
  ~Impl() {
    if (initialized) AudioUnitUninitialize(unit);
    if (unit) AudioComponentInstanceDispose(unit);
  }
  void requireEditable() const {
    if (consumed) throw std::logic_error("create a new piano instance to change state or render again");
  }
};

PianoteqAU::PianoteqAU() : impl_(std::make_unique<Impl>()) {
  AudioComponentDescription description{kAudioUnitType_MusicDevice, kSubtype, kManufacturer, 0, 0};
  impl_->component = AudioComponentFindNext(nullptr, &description);
  if (!impl_->component) throw std::runtime_error("Pianoteq 9 AU is not registered; install the local AU component first");
  check(AudioComponentInstanceNew(impl_->component, &impl_->unit), "load Pianoteq 9 AU");
}
PianoteqAU::~PianoteqAU() = default;

std::uint32_t PianoteqAU::componentVersion() const {
  UInt32 version = 0;
  check(AudioComponentGetVersion(impl_->component, &version), "read AU version");
  return version;
}

std::vector<std::string> PianoteqAU::factoryPresets() const {
  CFHandle<CFArrayRef> array;
  UInt32 size = sizeof(array.value);
  check(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global,
                            0, &array.value, &size), "list AU factory presets");
  if (!array.value) throw std::runtime_error("AU returned no preset array");
  std::vector<std::string> names;
  for (CFIndex i = 0; i < CFArrayGetCount(array.value); ++i) {
    const auto* preset = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(array.value, i));
    if (!preset) throw std::runtime_error("AU returned an invalid preset entry");
    names.push_back(utf8(preset->presetName));
  }
  return names;
}

void PianoteqAU::selectFactoryPreset(const std::string& name) {
  impl_->requireEditable();
  CFHandle<CFArrayRef> array;
  UInt32 size = sizeof(array.value);
  check(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global,
                            0, &array.value, &size), "list AU factory presets");
  if (!array.value) throw std::runtime_error("AU returned no preset array");
  for (CFIndex i = 0; i < CFArrayGetCount(array.value); ++i) {
    const auto* preset = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(array.value, i));
    if (preset && utf8(preset->presetName) == name) {
      check(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global,
                                0, preset, sizeof(*preset)), "select AU factory preset");
      return;
    }
  }
  throw std::invalid_argument("Pianoteq factory preset not found: " + name);
}

std::string PianoteqAU::presetName() const {
  AUPreset preset{};
  UInt32 size = sizeof(preset);
  check(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global,
                            0, &preset, &size), "read AU preset");
  CFHandle<CFStringRef> name;
  name.value = preset.presetName;
  return utf8(name.value);
}

std::vector<std::uint8_t> PianoteqAU::state() const {
  impl_->requireEditable();  // Snapshot the instrument, never post-performance voice state.
  CFHandle<CFPropertyListRef> dictionary;
  UInt32 size = sizeof(dictionary.value);
  check(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                            0, &dictionary.value, &size), "capture AU state");
  requireIdentity(dictionary.value);
  CFHandle<CFDataRef> data;
  data.value = CFPropertyListCreateData(nullptr, dictionary.value, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
  if (!data.value || CFDataGetLength(data.value) <= 0 ||
      static_cast<std::size_t>(CFDataGetLength(data.value)) > kStateLimit) {
    throw std::runtime_error("AU state serialization failed or exceeded 16 MiB");
  }
  const auto* bytes = CFDataGetBytePtr(data.value);
  return {bytes, bytes + CFDataGetLength(data.value)};
}

void PianoteqAU::restoreState(const std::vector<std::uint8_t>& bytes) {
  impl_->requireEditable();
  if (bytes.empty() || bytes.size() > kStateLimit) throw std::invalid_argument("AU state must contain 1..16 MiB bytes");
  CFHandle<CFDataRef> data;
  data.value = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
  if (!data.value) throw std::runtime_error("cannot allocate AU state");
  CFHandle<CFPropertyListRef> dictionary;
  dictionary.value = CFPropertyListCreateWithData(nullptr, data.value, kCFPropertyListImmutable, nullptr, nullptr);
  requireIdentity(dictionary.value);
  // Apple specifies the document property first when reopening a saved session.
  const auto status = AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_ClassInfoFromDocument,
      kAudioUnitScope_Global, 0, &dictionary.value, sizeof(dictionary.value));
  if (status != noErr) {
    check(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                              0, &dictionary.value, sizeof(dictionary.value)), "restore AU state");
  }
}

AudioBuffer PianoteqAU::render(const MidiFile& midi, PianoRenderReport* report, std::uint32_t rate, double tail) {
  impl_->requireEditable();
  const auto sequence = makeMidiSampleSequence(midi, rate, tail);
  AudioBuffer output;
  output.sample_rate = rate;
  output.channels = 2;
  output.samples.resize(sequence.frames * 2);
  impl_->consumed = true;
  AudioStreamBasicDescription format{};
  format.mSampleRate = rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
  format.mBytesPerPacket = sizeof(float);
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(float);
  format.mChannelsPerFrame = 2;
  format.mBitsPerChannel = 32;
  check(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                            0, &format, sizeof(format)), "configure AU stereo output");
  check(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                            0, &kBlock, sizeof(kBlock)), "configure AU block size");
  const UInt32 offline = 1;
  check(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global,
                            0, &offline, sizeof(offline)), "configure AU offline mode");
  check(AudioUnitInitialize(impl_->unit), "initialize AU");
  impl_->initialized = true;
  std::array<float, kBlock> left{}, right{};
  struct StereoBuffers { UInt32 count; ::AudioBuffer buffers[2]; } buffers{};
  PianoRenderReport diagnostics;
  double energy = 0.0, tail_energy = 0.0;
  const auto tail_frames = std::min<std::size_t>(rate, sequence.frames);
  std::size_t next = 0;
  for (std::size_t frame = 0; frame < sequence.frames; frame += kBlock) {
    const auto frames = static_cast<UInt32>(std::min<std::size_t>(kBlock, sequence.frames - frame));
    while (next < sequence.events.size() && sequence.events[next].frame < frame + frames) {
      const auto& event = sequence.events[next++];
      if (isInstrumentSelection(event)) { ++diagnostics.skipped_instrument_selection; continue; }
      check(MusicDeviceMIDIEvent(impl_->unit, event.status, event.data1, event.data2,
                                static_cast<UInt32>(event.frame - frame)), "send AU MIDI event");
      ++diagnostics.sent_messages;
    }
    left.fill(0.0F);
    right.fill(0.0F);
    const auto byte_count = static_cast<UInt32>(frames * sizeof(float));
    buffers = {2, {{1, byte_count, left.data()}, {1, byte_count, right.data()}}};
    AudioTimeStamp timestamp{};
    timestamp.mSampleTime = static_cast<Float64>(frame);
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    check(AudioUnitRender(impl_->unit, &flags, &timestamp, 0, frames,
                          reinterpret_cast<AudioBufferList*>(&buffers)), "render AU audio");
    if (buffers.count != 2) throw std::runtime_error("AU changed its output channel layout");
    for (std::size_t channel = 0; channel < 2; ++channel) {
      const auto& buffer = buffers.buffers[channel];
      if (buffer.mNumberChannels != 1 || !buffer.mData || buffer.mDataByteSize < byte_count) {
        throw std::runtime_error("AU returned an invalid output buffer");
      }
      const auto* samples = static_cast<const float*>(buffer.mData);
      for (UInt32 i = 0; i < frames; ++i) {
        const float value = samples[i];
        if (!std::isfinite(value)) throw std::runtime_error("AU produced a non-finite sample");
        diagnostics.peak = std::max(diagnostics.peak, std::abs(static_cast<double>(value)));
        const double square = static_cast<double>(value) * value;
        energy += square;
        if (frame + i >= sequence.frames - tail_frames) tail_energy += square;
        if (value < -1.0F || value > 1.0F) ++diagnostics.clipped_samples;
        output.samples[(frame + i) * 2 + channel] = std::clamp(value, -1.0F, 1.0F);
      }
    }
  }
  diagnostics.rms = std::sqrt(energy / static_cast<double>(output.samples.size()));
  diagnostics.last_second_rms = std::sqrt(tail_energy / static_cast<double>(2 * tail_frames));
  if (report) *report = diagnostics;
  return output;
}

}  // namespace daw
