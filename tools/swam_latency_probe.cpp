#include "audio_unit_runtime.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Opt-in diagnostic only: owns an isolated AU, never opens an output device,
// saves a preset, or relaxes AudioUnitInstrument's realtime admission policy.
namespace {
void check(OSStatus s, const char* action) {
  if (s) throw std::runtime_error(std::string(action) + " status=" + std::to_string(s));
}
std::string text(CFStringRef s) {
  if (!s) return {};
  std::vector<char> b(static_cast<std::size_t>(CFStringGetMaximumSizeForEncoding(
      CFStringGetLength(s), kCFStringEncodingUTF8)) + 1);
  if (!CFStringGetCString(s, b.data(), static_cast<CFIndex>(b.size()), kCFStringEncodingUTF8))
    throw std::runtime_error("CFString conversion failed");
  return b.data();
}
void releaseInfo(const AudioUnitParameterInfo& info) {
  if (info.flags & kAudioUnitParameterFlag_CFNameRelease) {
    if (info.cfNameString) CFRelease(info.cfNameString);
    if (info.unit == kAudioUnitParameterUnit_CustomUnit && info.unitName) CFRelease(info.unitName);
  }
}
struct Unit {
  std::atomic<unsigned> notifications{0};
  AudioUnit au = nullptr;
  bool initialized = false, listening = false;
  static void changed(void* p, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {
    static_cast<Unit*>(p)->notifications.fetch_add(1, std::memory_order_relaxed);
  }
  ~Unit() {
    // Keep listener context alive through uninitialization and disposal too.
    if (listening) AudioUnitRemovePropertyListenerWithUserData(au, kAudioUnitProperty_Latency, changed, this);
    if (initialized) AudioUnitUninitialize(au);
    if (au) AudioComponentInstanceDispose(au);
  }
  double latency() const {
    Float64 n = -1; UInt32 size = sizeof(n);
    check(AudioUnitGetProperty(au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &n, &size), "read latency");
    if (size != sizeof(n) || !std::isfinite(n) || n < 0) throw std::runtime_error("invalid latency");
    return n;
  }
};
}
int main(int argc, char** argv) {
  try {
    // Usage: [--offline] [parameter-id value]...; no implicit parameter sweep.
    int first = 1; UInt32 offline = 0;
    if (argc > 1 && std::string(argv[1]) == "--offline") { offline = 1; ++first; }
    if ((argc - first) % 2) throw std::invalid_argument("expected parameter-id/value pairs");
    daw::prepareAudioUnitRuntime();
    Unit u;
    AudioComponentDescription d{kAudioUnitType_MusicDevice, 'Sce3', 'AuMo', 0, 0};
    auto component = AudioComponentFindNext(nullptr, &d);
    if (!component) throw std::runtime_error("SWAM Cello 3 not installed");
    UInt32 version = 0; check(AudioComponentGetVersion(component, &version), "version");
    check(AudioComponentInstanceNew(component, &u.au), "create SWAM");
    check(AudioUnitAddPropertyListener(u.au, kAudioUnitProperty_Latency, Unit::changed, &u), "listen latency");
    u.listening = true;
    AudioStreamBasicDescription f{};
    f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    const UInt32 block = 256;
    check(AudioUnitSetProperty(u.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof(f)), "format");
    check(AudioUnitSetProperty(u.au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &block, sizeof(block)), "slice");
    check(AudioUnitSetProperty(u.au, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offline, sizeof(offline)), "mode");
    check(AudioUnitInitialize(u.au), "initialize"); u.initialized = true;
    daw::serviceAudioUnitRuntime(5);
    std::cout << std::setprecision(17) << "component=aumu/Sce3/AuMo version=" << version
              << " sample_rate=48000 offline=" << offline << " output_device=none\n";
    const double baseline = u.latency(); bool stable = true; std::uint64_t frame = 0;
    auto observe = [&](const std::string& label) {
      const double before = u.latency(); double peak = 0;
      check(MusicDeviceMIDIEvent(u.au, 0xB0, 11, 100, 0), "CC11");
      check(MusicDeviceMIDIEvent(u.au, 0x90, 60, 90, 0), "note on");
      // Exercise processing, not just a getter. These are offline-driven render
      // calls even when OfflineRender=0: no realtime deadline is certified here.
      for (unsigned k = 0; k < 20; ++k) {
        std::array<float, 256> l{}, r{};
        struct Buffers { UInt32 n; AudioBuffer b[2]; } buffers{2, {{1, 1024, l.data()}, {1, 1024, r.data()}}};
        AudioTimeStamp ts{}; ts.mSampleTime = static_cast<double>(frame); ts.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags = 0;
        check(AudioUnitRender(u.au, &flags, &ts, 0, block, reinterpret_cast<AudioBufferList*>(&buffers)), "render");
        if (buffers.n != 2) throw std::runtime_error("unexpected channel layout");
        for (const auto& b : buffers.b) {
          if (!b.mData || b.mNumberChannels != 1 || b.mDataByteSize < 1024) throw std::runtime_error("invalid buffer");
          const auto* samples = static_cast<const float*>(b.mData);
          for (unsigned i = 0; i < 256; ++i) {
            if (!std::isfinite(samples[i])) throw std::runtime_error("nonfinite output");
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
          }
        }
        frame += block;
        const auto current = u.latency();
        if (current != baseline) {
          stable = false;
          std::cout << "latency_difference frame=" << frame << " seconds=" << current << '\n';
        }
      }
      check(MusicDeviceMIDIEvent(u.au, 0x80, 60, 0, 0), "note off");
      daw::serviceAudioUnitRuntime(0.15);
      const double after = u.latency();
      stable = stable && before == baseline && after == baseline;
      std::cout << "observation " << std::quoted(label) << " before_seconds=" << before
                << " after_seconds=" << after << " notifications=" << u.notifications.load()
                << " peak=" << peak << '\n' << std::flush;
    };
    observe("initialized");
    CFArrayRef presets = nullptr; UInt32 size = sizeof(presets);
    check(AudioUnitGetProperty(u.au, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size), "factory presets");
    // Copy names/numbers before changing presets, release the property array.
    std::vector<std::pair<SInt32, std::string>> choices;
    if (presets) {
      for (CFIndex i = 0; i < CFArrayGetCount(presets); ++i) {
        const auto* p = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, i));
        choices.emplace_back(p->presetNumber, text(p->presetName));
      }
      CFRelease(presets);
    }
    std::cout << "factory_preset_count=" << choices.size() << '\n';
    for (const auto& p : choices) {
      CFStringRef name = CFStringCreateWithCString(nullptr, p.second.c_str(), kCFStringEncodingUTF8);
      AUPreset preset{p.first, name};
      auto status = AudioUnitSetProperty(u.au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &preset, sizeof(preset));
      CFRelease(name); check(status, "select factory preset");
      daw::serviceAudioUnitRuntime(0.15);
      AUPreset readback{}; size = sizeof(readback);
      check(AudioUnitGetProperty(u.au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &readback, &size), "preset readback");
      const auto actual = text(readback.presetName);
      if (readback.presetName) CFRelease(readback.presetName);
      std::cout << "preset requested=" << std::quoted(p.second) << " readback=" << std::quoted(actual) << '\n';
      if (actual != p.second) throw std::runtime_error("preset readback mismatch");
      observe("factory:" + p.second);
    }
    Boolean writable = false; size = 0;
    check(AudioUnitGetPropertyInfo(u.au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, &writable), "parameter list size");
    if (size % sizeof(AudioUnitParameterID) || size > 1024 * sizeof(AudioUnitParameterID)) throw std::runtime_error("invalid parameter list size");
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    check(AudioUnitGetProperty(u.au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size), "parameters");
    for (auto id : ids) {
      AudioUnitParameterInfo info{}; size = sizeof(info);
      check(AudioUnitGetProperty(u.au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size), "parameter info");
      const auto name = info.cfNameString ? text(info.cfNameString) : std::string(info.name);
      releaseInfo(info);
      AudioUnitParameterValue value = 0; check(AudioUnitGetParameter(u.au, id, kAudioUnitScope_Global, 0, &value), "parameter value");
      std::cout << "parameter id=" << id << " name=" << std::quoted(name) << " min=" << info.minValue
                << " max=" << info.maxValue << " value=" << value << " flags=" << info.flags << '\n';
      CFArrayRef strings = nullptr; size = sizeof(strings);
      if (AudioUnitGetProperty(u.au, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &strings, &size) == noErr && strings) {
        for (CFIndex n = 0; n < CFArrayGetCount(strings); ++n)
          std::cout << "value_label id=" << id << " index=" << n << " label="
                    << std::quoted(text(static_cast<CFStringRef>(CFArrayGetValueAtIndex(strings, n)))) << '\n';
        CFRelease(strings);
      }
    }
    for (int i = first; i < argc; i += 2) {
      std::size_t used = 0; auto raw = std::stoul(argv[i], &used);
      if (used != std::string(argv[i]).size() || raw > UINT32_MAX) throw std::invalid_argument("invalid parameter id");
      auto id = static_cast<AudioUnitParameterID>(raw);
      auto value = std::stof(argv[i + 1], &used);
      if (used != std::string(argv[i + 1]).size() || !std::isfinite(value)) throw std::invalid_argument("invalid value");
      AudioUnitParameterInfo info{}; size = sizeof(info);
      check(AudioUnitGetProperty(u.au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size), "sweep info");
      releaseInfo(info);
      if (!(info.flags & kAudioUnitParameterFlag_IsWritable) || value < info.minValue || value > info.maxValue) throw std::invalid_argument("unsupported parameter value");
      check(AudioUnitSetParameter(u.au, id, kAudioUnitScope_Global, 0, value, 0), "set parameter");
      daw::serviceAudioUnitRuntime(0.15);
      AudioUnitParameterValue actual = 0;
      check(AudioUnitGetParameter(u.au, id, kAudioUnitScope_Global, 0, &actual), "readback");
      std::cout << "change id=" << id << " requested=" << value << " readback=" << actual << '\n';
      if (actual != value) throw std::runtime_error("parameter readback mismatch");
      observe("parameter:" + std::to_string(id) + "=" + std::to_string(value));
    }
    std::cout << "observed_latency_stable=" << stable << " baseline_seconds=" << baseline
              << " notifications=" << u.notifications.load() << " coverage=listed_settings_only\n";
    return stable ? 0 : 2;
  } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
