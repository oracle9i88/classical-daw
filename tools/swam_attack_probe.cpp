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
#include <filesystem>
#include <fstream>

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

struct Case { const char* name; unsigned velocity; AudioUnitParameterID id; float value; const char* parameter_name; };
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("usage: daw_swam_attack_probe NEW_OUTPUT_DIRECTORY");
    const std::filesystem::path directory(argv[1]);
    if (!std::filesystem::create_directory(directory)) throw std::invalid_argument("output directory must be new");
    daw::prepareAudioUnitRuntime();
    std::ofstream manifest(directory/"manifest.tsv");
    manifest << "case\tpitch\tvelocity\trepeat\tparameter_id\tparameter_value\tlatency_seconds\tnotifications\tfile\n";
    const Case cases[] = {
      {"bow-default",96,2008574934,0,"Play Mode"},
      {"bow-velocity32",32,2008574934,0,"Play Mode"},
      {"bow-velocity127",127,2008574934,0,"Play Mode"},
      {"bow-ramp0",96,641237175,0,"Attack Ramp Speed"},
      {"bow-ramp1",96,641237175,1,"Attack Ramp Speed"},
      {"bow-position0",96,1013107514,0,"Bow/Pizz Position"},
      {"bow-position1",96,1013107514,1,"Bow/Pizz Position"},
      {"bow-pressure0.1",96,1484578252,.1F,"Bow Pressure"},
      {"bow-pressure0.9",96,1484578252,.9F,"Bow Pressure"},
      {"bow-sordino",96,1336834641,1,"Sordino"},
      {"pizzicato",96,2008574934,1,"Play Mode"},
      {"col-legno",96,2008574934,2,"Play Mode"}
    };
    for (const auto& c : cases) for (unsigned pitch : {48U,60U}) for (unsigned repeat : {1U,2U}) {
      Unit u;
      AudioComponentDescription d{kAudioUnitType_MusicDevice,'Sce3','AuMo',0,0};
      auto component=AudioComponentFindNext(nullptr,&d);
      if(!component) throw std::runtime_error("missing SWAM");
      UInt32 version=0; check(AudioComponentGetVersion(component,&version),"version");
      if(version!=199682) throw std::runtime_error("case definitions require inspected SWAM 3.12.2; re-enumerate parameters");
      check(AudioComponentInstanceNew(component,&u.au),"create");
      check(AudioUnitAddPropertyListener(u.au,kAudioUnitProperty_Latency,Unit::changed,&u),"listener"); u.listening=true;
      CFArrayRef presets=nullptr; UInt32 size=sizeof(presets);
      check(AudioUnitGetProperty(u.au,kAudioUnitProperty_FactoryPresets,kAudioUnitScope_Global,0,&presets,&size),"presets");
      bool selected=false;
      if(presets) {
        for(CFIndex i=0;i<CFArrayGetCount(presets);++i) {
          auto* p=static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets,i));
          if(text(p->presetName)=="Cello") { check(AudioUnitSetProperty(u.au,kAudioUnitProperty_PresentPreset,kAudioUnitScope_Global,0,p,sizeof(*p)),"Cello preset"); selected=true; break; }
        }
        CFRelease(presets);
      }
      if(!selected) throw std::runtime_error("Cello factory preset absent");
      {
      CFPropertyListRef state=nullptr; size=sizeof(state);
      check(AudioUnitGetProperty(u.au,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"state check");
      CFDataRef serialized=CFPropertyListCreateData(nullptr,state,kCFPropertyListBinaryFormat_v1_0,0,nullptr); CFRelease(state);
      if(!serialized) throw std::runtime_error("cannot validate transpose");
      std::vector<std::uint8_t> bytes(CFDataGetBytePtr(serialized),CFDataGetBytePtr(serialized)+CFDataGetLength(serialized)); CFRelease(serialized);
      const auto corrected = daw::swamCelloConcertPitchState(bytes);
      CFDataRef corrected_data=CFDataCreate(nullptr,corrected.data(),static_cast<CFIndex>(corrected.size()));
      CFPropertyListRef corrected_state=CFPropertyListCreateWithData(nullptr,corrected_data,kCFPropertyListImmutable,nullptr,nullptr);
      CFRelease(corrected_data);
      if(!corrected_state) throw std::runtime_error("invalid corrected state");
      auto restore=AudioUnitSetProperty(u.au,kAudioUnitProperty_ClassInfoFromDocument,kAudioUnitScope_Global,0,&corrected_state,sizeof(corrected_state));
      if(restore) restore=AudioUnitSetProperty(u.au,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&corrected_state,sizeof(corrected_state));
      CFRelease(corrected_state); check(restore,"restore concert-pitch state before initialization");
      }
      AudioStreamBasicDescription f{}; f.mSampleRate=48000; f.mFormatID=kAudioFormatLinearPCM;
      f.mFormatFlags=kAudioFormatFlagsNativeFloatPacked|kAudioFormatFlagIsNonInterleaved;
      f.mBytesPerPacket=f.mBytesPerFrame=4; f.mFramesPerPacket=1; f.mChannelsPerFrame=2; f.mBitsPerChannel=32;
      const UInt32 block=256, offline=1;
      check(AudioUnitSetProperty(u.au,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,sizeof(f)),"format");
      check(AudioUnitSetProperty(u.au,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&block,sizeof(block)),"slice");
      check(AudioUnitSetProperty(u.au,kAudioUnitProperty_OfflineRender,kAudioUnitScope_Global,0,&offline,sizeof(offline)),"offline");
      check(AudioUnitInitialize(u.au),"initialize"); u.initialized=true;
      daw::serviceAudioUnitRuntime(5);
      auto set=[&](AudioUnitParameterID id,float value,const char* expected_name) {
        AudioUnitParameterInfo info{}; size=sizeof(info);
        check(AudioUnitGetProperty(u.au,kAudioUnitProperty_ParameterInfo,kAudioUnitScope_Global,id,&info,&size),"parameter metadata");
        const auto name=text(info.cfNameString); releaseInfo(info);
        if(name!=expected_name || value<info.minValue || value>info.maxValue) throw std::runtime_error("parameter definition mismatch");
        check(AudioUnitSetParameter(u.au,id,kAudioUnitScope_Global,0,value,0),"set parameter");
        daw::serviceAudioUnitRuntime(.05);
        float actual=0; check(AudioUnitGetParameter(u.au,id,kAudioUnitScope_Global,0,&actual),"readback");
        if(std::abs(actual-value)>1e-6F) throw std::runtime_error("parameter readback mismatch");
        std::cout<<"setting id="<<id<<" name="<<std::quoted(name)<<" requested="<<value<<" actual="<<actual<<'\n';
      };
      // Validate the same serialized transpose schema as production, after
      // restoring concert-pitch state BEFORE initialization (not an async UI write).
      {
      CFPropertyListRef state=nullptr; size=sizeof(state);
      check(AudioUnitGetProperty(u.au,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"state check");
      CFDataRef serialized=CFPropertyListCreateData(nullptr,state,kCFPropertyListBinaryFormat_v1_0,0,nullptr); CFRelease(state);
      if(!serialized) throw std::runtime_error("cannot validate transpose");
      std::vector<std::uint8_t> bytes(CFDataGetBytePtr(serialized),CFDataGetBytePtr(serialized)+CFDataGetLength(serialized)); CFRelease(serialized);
      if(daw::swamCelloStateTranspose(bytes)!=0) throw std::runtime_error("concert-pitch state was not retained");
      }
      set(1099171302,0,"Ambiente\nRoom Simulator");
      set(655530384,0,"Source Delay Mode");
      set(c.id,c.value,c.parameter_name);
      const double latency=u.latency(); const auto notifications=u.notifications.load();
      const auto filename=std::string(c.name)+"-"+std::to_string(pitch)+"-"+std::to_string(repeat)+".f32le";
      std::ofstream audio(directory/filename,std::ios::binary);
      constexpr unsigned total=240000, onset=48000, release=192000;
      double peak=0;
      for(unsigned frame=0;frame<total;frame+=block) {
        const auto n=std::min(block,total-frame);
        if(frame==0) check(MusicDeviceMIDIEvent(u.au,0xb0,11,100,0),"expression");
        if(frame<=onset && onset<frame+n) check(MusicDeviceMIDIEvent(u.au,0x90,pitch,c.velocity,onset-frame),"attack");
        if(frame<=release && release<frame+n) check(MusicDeviceMIDIEvent(u.au,0x80,pitch,0,release-frame),"release");
        std::array<float,256> l{},r{}; std::array<float,512> stereo{};
        struct Buffers { UInt32 count; AudioBuffer buffers[2]; } b{2,{{1,n*4,l.data()},{1,n*4,r.data()}}};
        AudioTimeStamp ts{}; ts.mSampleTime=frame; ts.mFlags=kAudioTimeStampSampleTimeValid; AudioUnitRenderActionFlags flags=0;
        check(AudioUnitRender(u.au,&flags,&ts,0,n,reinterpret_cast<AudioBufferList*>(&b)),"render");
        if(b.count!=2) throw std::runtime_error("invalid output layout");
        for(unsigned channel=0;channel<2;++channel) {
          const auto& buffer=b.buffers[channel];
          if(!buffer.mData || buffer.mDataByteSize<n*4 || buffer.mNumberChannels!=1) throw std::runtime_error("invalid buffer");
          const auto* data=static_cast<const float*>(buffer.mData);
          for(unsigned i=0;i<n;++i) {
            if(!std::isfinite(data[i])) throw std::runtime_error("nonfinite audio");
            stereo[i*2+channel]=data[i]; peak=std::max(peak,std::abs(static_cast<double>(data[i])));
          }
        }
        audio.write(reinterpret_cast<const char*>(stereo.data()),n*2*sizeof(float));
        if(u.latency()!=latency) throw std::runtime_error("latency changed during attack capture");
      }
      audio.close(); if(!audio || peak==0) throw std::runtime_error("empty/failed audio capture");
      manifest<<std::setprecision(17)<<c.name<<'\t'<<pitch<<'\t'<<c.velocity<<'\t'<<repeat<<'\t'<<c.id<<'\t'<<c.value<<'\t'<<latency<<'\t'<<u.notifications.load()<<'\t'<<filename<<'\n'; manifest.flush();
      if(!manifest) throw std::runtime_error("manifest write failed");
      std::cout<<"case="<<c.name<<" pitch="<<pitch<<" repeat="<<repeat<<" latency="<<latency<<" notifications_before="<<notifications<<" notifications_after="<<u.notifications.load()<<" peak="<<peak<<" file="<<filename<<'\n'<<std::flush;
    }
    std::cout<<"PASS captures=48 output_device=none saved_preset=none coverage=listed_settings_only\n";
  } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
