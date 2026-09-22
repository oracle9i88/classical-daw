#include "daw/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr double kSampleRate = 24000.0;
using daw::AudioBuffer;
using daw::MidiChannelEvent;
using daw::MidiChannelEventType;
using daw::MidiFile;
using daw::MidiNote;
using daw::Tick;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

MidiNote note(Tick start, Tick duration, std::uint8_t channel = 0,
              std::uint8_t pitch = 69) {
  MidiNote result;
  result.start = start;
  result.duration = duration;
  result.pitch = pitch;
  result.velocity = 100;
  result.channel = channel;
  return result;
}

MidiChannelEvent cc(Tick tick, std::uint8_t controller, std::uint8_t value,
                    std::uint8_t channel = 0, std::uint64_t order = 0) {
  return {tick, MidiChannelEventType::ControlChange, channel, controller, value, order};
}

MidiChannelEvent bend(Tick tick, std::uint16_t value, std::uint8_t channel = 0) {
  return {tick, MidiChannelEventType::PitchBend, channel,
          static_cast<std::uint8_t>(value & 127),
          static_cast<std::uint8_t>(value >> 7), 0};
}

MidiFile fixture(Tick duration = 1920) {
  MidiFile result;
  result.tempo = daw::TempoMap(60.0);  // 960 ticks per second.
  result.tracks.push_back({"Notes", {note(0, duration)}, {}});
  return result;
}

AudioBuffer render(const MidiFile& file, daw::MidiRenderReport* report = nullptr,
                   double tail = 0.1) {
  return daw::renderMidiFile(file, kSampleRate, tail, report);
}

std::size_t frame(double seconds) {
  return static_cast<std::size_t>(std::llround(seconds * kSampleRate));
}

double rms(const AudioBuffer& audio, double from, double to) {
  const auto first = frame(from);
  const auto end = frame(to);
  require(first < end && end <= audio.samples.size(), "invalid measurement window");
  double sum = 0.0;
  for (std::size_t i = first; i < end; ++i) {
    require(std::isfinite(audio.samples[i]), "renderer produced a non-finite sample");
    sum += static_cast<double>(audio.samples[i]) * audio.samples[i];
  }
  return std::sqrt(sum / static_cast<double>(end - first));
}

void audible(const AudioBuffer& audio, double from, double to, const std::string& reason) {
  require(rms(audio, from, to) > 0.005, reason);
}

void silent(const AudioBuffer& audio, double from, double to, const std::string& reason) {
  require(rms(audio, from, to) < 1.0e-7, reason);
}

// Measure the frequency from interpolated positive-going crossings, without
// sharing oscillator or pitch-to-frequency code with the production renderer.
double frequency(const AudioBuffer& audio, double from, double to) {
  double first = 0.0;
  double last = 0.0;
  std::size_t crossings = 0;
  const auto end = frame(to);
  require(frame(from) > 0 && end <= audio.samples.size(), "invalid frequency window");
  for (std::size_t i = frame(from); i < end; ++i) {
    const double previous = audio.samples[i - 1];
    const double current = audio.samples[i];
    if (previous <= 0.0 && current > 0.0) {
      const double position = static_cast<double>(i - 1) - previous / (current - previous);
      if (crossings == 0) first = position;
      last = position;
      ++crossings;
    }
  }
  require(crossings > 10, "too few audible crossings to measure frequency");
  return static_cast<double>(crossings - 1) * kSampleRate / (last - first);
}

void near(double actual, double expected, double tolerance, const std::string& reason) {
  require(std::abs(actual - expected) <= tolerance,
          reason + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

void releaseAndSustain() {
  auto plain = fixture(480);
  const auto dry = render(plain);
  require(dry.sample_rate == 24000 && dry.channels == 1 && dry.frameCount() == 14400,
          "render shape must include the requested tail");
  audible(dry, 0.505, 0.52, "key release must decay after, not before, note-off");
  silent(dry, 0.54, 0.59, "released note did not finish its 35 ms decay");

  auto sustained = plain;
  sustained.tracks[0].channel_events = {cc(0, 64, 64), cc(960, 64, 0)};
  const auto held = render(sustained);
  audible(held, 0.65, 0.9, "CC64 value 64 did not hold a released key");
  audible(held, 1.005, 1.02, "pedal-up must begin an audible release tail");
  silent(held, 1.04, 1.09, "pedal-up did not release sustained sound");
  sustained.tracks[0].channel_events[0].data2 = 63;
  const auto below_threshold = render(sustained);
  silent(below_threshold, 0.65, 0.9, "CC64 value 63 incorrectly enabled sustain");

  auto final_pedal = plain;
  final_pedal.tracks[0].channel_events = {cc(0, 64, 127)};
  daw::MidiRenderReport report;
  const auto final_release = render(final_pedal, &report);
  require(report.voices_released_at_end == 1, "held pedal at source end was not reported");
  audible(final_release, 0.505, 0.52, "held pedal at source end should release into the tail");
  silent(final_release, 0.54, 0.59, "held pedal left an endless note after source end");
  require(render(final_pedal, nullptr, 0.0).frameCount() == 12000,
          "an explicitly zero tail must not silently add release duration");
}

void volumeAndExpression() {
  const auto baseline = render(fixture());
  auto changed = fixture();
  changed.tracks[0].channel_events = {cc(480, 7, 64), cc(960, 11, 32), cc(1440, 7, 0)};
  const auto audio = render(changed);
  near(rms(audio, 0.1, 0.4) / rms(baseline, 0.1, 0.4), 1.0, 0.001,
       "a future controller changed earlier sound");
  near(rms(audio, 0.6, 0.9) / rms(baseline, 0.6, 0.9), 64.0 / 127.0, 0.001,
       "CC7 must affect an already sounding note linearly");
  near(rms(audio, 1.1, 1.4) / rms(baseline, 1.1, 1.4), 64.0 * 32.0 / (127.0 * 127.0), 0.001,
       "CC11 must multiply the current channel volume");
  silent(audio, 1.6, 1.9, "CC7 zero did not mute an active voice");
}

void channelsAcrossTracks() {
  auto shared = fixture(960);
  shared.tracks[0].notes.push_back(note(0, 960, 1));
  shared.tracks.push_back({"Controller only", {}, {cc(480, 7, 0, 0)}});
  const auto mixed = render(shared);
  auto single = fixture(960);
  single.tracks[0].notes[0].channel = 1;
  const auto reference = render(single);
  near(rms(mixed, 0.1, 0.4) / rms(reference, 0.1, 0.4), 2.0, 0.001,
       "independent channels should both sound before control change");
  near(rms(mixed, 0.6, 0.9) / rms(reference, 0.6, 0.9), 1.0, 0.001,
       "a controller-only track must affect its channel in other tracks, without muting channel 1");

  auto pedal = fixture(240);
  pedal.tracks[0].notes.push_back(note(0, 240, 1, 72));
  pedal.tracks.push_back({"Pedal only", {}, {cc(0, 64, 127), cc(960, 64, 0)}});
  auto expected = pedal;
  expected.tracks[0].notes.erase(expected.tracks[0].notes.begin() + 1);
  const auto actual_audio = render(pedal);
  const auto expected_audio = render(expected);
  audible(actual_audio, 0.4, 0.8, "cross-track sustain did not hold its channel");
  for (std::size_t i = frame(0.4); i < frame(0.8); ++i) {
    near(actual_audio.samples[i], expected_audio.samples[i], 1.0e-6,
         "sustain leaked from channel 0 to channel 1");
  }
}

void pitchBend() {
  auto file = fixture(2400);
  file.tracks[0].channel_events = {bend(480, 16383), bend(960, 0), bend(1440, 8192), bend(1920, 16383, 1)};
  const auto audio = render(file);
  near(frequency(audio, 0.1, 0.4), 440.0, 0.02, "default pitch must be A4");
  near(frequency(audio, 0.6, 0.9), 493.883301, 0.02, "maximum bend must reach two semitones up");
  near(frequency(audio, 1.1, 1.4), 391.995436, 0.02, "minimum bend must reach two semitones down");
  near(frequency(audio, 1.6, 1.9), 440.0, 0.02, "bend center did not restore original pitch");
  near(frequency(audio, 2.1, 2.4), 440.0, 0.02, "pitch bend affected the wrong channel");
}

void channelModeMessages() {
  auto all_off = fixture();
  all_off.tracks[0].channel_events = {cc(0, 64, 127), cc(480, 123, 0), cc(960, 64, 0)};
  auto audio = render(all_off);
  audible(audio, 0.65, 0.9, "all notes off must honor an already held sustain pedal");
  silent(audio, 1.05, 1.2, "pedal-up did not release keys previously released by all notes off");
  all_off.tracks[0].channel_events.erase(all_off.tracks[0].channel_events.begin());
  audio = render(all_off);
  audible(audio, 0.505, 0.52, "all notes off without pedal must use a release tail");
  silent(audio, 0.55, 0.8, "all notes off without sustain failed to release a key");

  auto sound_off = fixture();
  sound_off.tracks[0].channel_events = {cc(0, 64, 127), cc(480, 120, 0)};
  sound_off.tracks[0].notes.push_back(note(720, 240, 0, 72));
  audio = render(sound_off);
  silent(audio, 0.5005, 0.7, "all sound off must silence immediately, despite sustain");
  audible(audio, 0.8, 0.9, "all sound off must not prevent later note-ons");

  auto reset = fixture(240);
  reset.tracks[0].notes.push_back(note(720, 720));
  reset.tracks[0].channel_events = {
      cc(0, 7, 64), cc(0, 11, 32), bend(0, 16383), cc(0, 64, 127), cc(480, 121, 0)};
  audio = render(reset);
  audible(audio, 0.3, 0.45, "reset fixture should have a pedal-held note");
  silent(audio, 0.55, 0.7, "reset all controllers must release pedal-held notes");
  near(frequency(audio, 0.9, 1.3), 440.0, 0.02, "reset all controllers did not center pitch bend");
  auto reset_reference = fixture(240);
  reset_reference.tracks[0].notes.push_back(note(720, 720));
  const auto reference = render(reset_reference);
  near(rms(audio, 0.9, 1.3) / rms(reference, 0.9, 1.3), 64.0 / 127.0, 0.001,
       "reset controllers must restore expression but retain channel volume");
}

void sameTickOrdering() {
  auto imported = fixture(480);
  imported.tracks[0].notes[0].on_order = 1;
  imported.tracks[0].notes[0].off_order = 3;
  imported.tracks[0].channel_events = {cc(480, 64, 127, 0, 2), cc(960, 64, 0, 0, 4)};
  audible(render(imported), 0.65, 0.9,
          "source order must allow pedal-down before a same-tick note-off");
  imported.tracks[0].notes[0].off_order = 2;
  imported.tracks[0].channel_events[0].order = 3;
  silent(render(imported), 0.65, 0.9,
         "pedal-down after source note-off must not revive an already released key");

  auto authored = fixture(480);
  authored.tracks[0].channel_events = {cc(480, 64, 127), cc(960, 64, 0)};
  silent(render(authored), 0.65, 0.9,
         "newly authored same-tick note-off should precede controller events");

  auto cross_track = fixture(480);
  cross_track.tracks.push_back({"Pedal", {}, {cc(480, 64, 127), cc(960, 64, 0)}});
  silent(render(cross_track), 0.65, 0.9,
         "same-tick events must use a deterministic track-index order");
  std::swap(cross_track.tracks[0], cross_track.tracks[1]);
  audible(render(cross_track), 0.65, 0.9,
          "a lower-index controller track must precede higher-index same-tick note-offs");
}

void tempoAndDiagnostics() {
  auto file = fixture(960);
  file.tempo = daw::TempoMap(120.0);
  file.tempo.addChange(480, 60.0);
  file.tracks[0].channel_events = {cc(0, 64, 127), cc(1440, 64, 0)};
  daw::MidiRenderReport report;
  const auto audio = render(file, &report);
  // Tick 480 is 0.25 s; tick 1440 is 1.25 s after the tempo slows.
  require(audio.frameCount() == 32400, "event-only ending did not use the complete tempo map");
  audible(audio, 0.9, 1.15, "tempo change scheduled sustained release too early");
  silent(audio, 1.29, 1.34, "tempo change scheduled sustained release too late");
  require(report.interpreted_channel_events == 2 && report.unsupported_channel_events == 0 &&
              report.voices_released_at_end == 0,
          "ordinary controller accounting or pedal-up ending diagnostics are incorrect");

  auto unsupported = fixture(960);
  unsupported.tracks[0].channel_events = {
      {0, MidiChannelEventType::ProgramChange, 0, 41, 0, 0},
      {240, MidiChannelEventType::PolyPressure, 0, 69, 80, 0},
      {480, MidiChannelEventType::ChannelPressure, 0, 80, 0, 0},
      cc(720, 66, 127), cc(800, 7, 127)};
  const auto unchanged = render(fixture(960));
  const auto unsupported_audio = render(unsupported, &report);
  require(report.interpreted_channel_events == 1 && report.unsupported_channel_events == 4,
          "unsupported program, pressure and sostenuto messages must be reported separately");
  require(unsupported_audio.samples.size() == unchanged.samples.size(), "ignored controls changed duration");
  for (std::size_t i = 0; i < unchanged.samples.size(); ++i) {
    near(unsupported_audio.samples[i], unchanged.samples[i], 1.0e-6,
         "unsupported messages silently changed the diagnostic instrument");
  }
}

void overlappingSamePitchNotes() {
  auto overlap = fixture(960);
  overlap.tracks[0].notes.push_back(note(480, 960));
  const auto audio = render(overlap);
  audible(audio, 1.1, 1.4,
          "the first note-off must not silence a second, still-held note of the same pitch");
  silent(audio, 1.54, 1.59, "overlapping same-pitch voices did not release by their own endpoints");

  auto retrigger = fixture(240);
  retrigger.tracks[0].notes.push_back(note(480, 960));
  retrigger.tracks[0].channel_events = {cc(0, 64, 127), cc(960, 64, 0)};
  const auto retrigger_audio = render(retrigger);
  audible(retrigger_audio, 1.1, 1.4,
          "pedal-up must release an old sustained voice without releasing a new held key of the same pitch");
  silent(retrigger_audio, 1.54, 1.59, "same-pitch pedal retrigger left a stuck voice");
}

void distinctTicksAtOneFrame() {
  // At 6000 BPM a tick lasts one quarter of an output sample. Both tick
  // 47999 and 48001 round to the note-off's sample, yet musical event order
  // must still decide whether the released key is caught by the pedal.
  auto file = fixture(48000);
  file.tempo = daw::TempoMap(6000.0);
  file.tracks.push_back({"Pedal", {}, {cc(47999, 64, 127), cc(96000, 64, 0)}});
  audible(render(file), 0.65, 0.9,
          "tick order was lost when an earlier pedal event rounded to the note-off sample");
  file.tracks[1].channel_events[0].tick = 48001;
  silent(render(file), 0.65, 0.9,
         "a later pedal event rounded to the note-off sample must not catch the released key");
}

void diagnosticsAndValidation() {
  auto released = fixture(960);
  released.tracks[0].notes[0].release_velocity = 90;
  daw::MidiRenderReport report;
  (void)render(released, &report);
  require(report.ignored_release_velocities == 1,
          "release velocity without a sound implementation must be reported");

  auto loud = fixture(240);
  loud.tracks[0].notes.assign(20, note(0, 240));
  const auto clipped = render(loud, &report);
  require(report.clipped_samples > 0, "clipping a large unison must produce a diagnostic");
  for (const auto sample : clipped.samples) {
    require(std::isfinite(sample) && sample >= -1.0F && sample <= 1.0F,
            "clipped output must remain finite and normalized");
  }

  const daw::MidiRenderReport sentinel{11, 22, 33, 44, 55};
  auto rejected = [&](const MidiFile& file, const std::string& reason,
                      double rate = kSampleRate, double tail = 0.1) {
    report = sentinel;
    bool threw = false;
    try {
      (void)daw::renderMidiFile(file, rate, tail, &report);
    } catch (const std::invalid_argument&) {
      threw = true;
    } catch (const std::length_error&) {
      threw = true;
    }
    require(threw, reason);
    require(report.interpreted_channel_events == sentinel.interpreted_channel_events &&
                report.unsupported_channel_events == sentinel.unsupported_channel_events &&
                report.voices_released_at_end == sentinel.voices_released_at_end &&
                report.ignored_release_velocities == sentinel.ignored_release_velocities &&
                report.clipped_samples == sentinel.clipped_samples,
            "failed rendering changed caller-owned diagnostics: " + reason);
  };
  auto invalid = fixture(960);
  invalid.ticks_per_quarter = 384;
  rejected(invalid, "unnormalized PPQ was accepted");
  invalid = fixture(960);
  invalid.format = 2;
  rejected(invalid, "asynchronous SMF Type 2 was accepted");
  invalid.format = 0;
  invalid.tracks.push_back({"Extra", {}, {}});
  rejected(invalid, "SMF Type 0 with multiple tracks was accepted");
  for (int field = 0; field < 8; ++field) {
    invalid = fixture(960);
    auto& bad_note = invalid.tracks[0].notes[0];
    switch (field) {
      case 0: bad_note.start = -1; break;
      case 1: bad_note.duration = 0; break;
      case 2: bad_note.pitch = 128; break;
      case 3: bad_note.velocity = 0; break;
      case 4: bad_note.velocity = 128; break;
      case 5: bad_note.channel = 16; break;
      case 6: bad_note.release_velocity = 128; break;
      case 7: bad_note.start = std::numeric_limits<Tick>::max(); break;
    }
    rejected(invalid, "out-of-range note field " + std::to_string(field) + " was accepted");
  }
  for (int field = 0; field < 6; ++field) {
    invalid = fixture(960);
    auto bad_event = cc(0, 7, 100);
    switch (field) {
      case 0: bad_event.tick = -1; break;
      case 1: bad_event.channel = 16; break;
      case 2: bad_event.data1 = 128; break;
      case 3: bad_event.data2 = 128; break;
      case 4: bad_event.type = MidiChannelEventType::ProgramChange; break;  // nonzero second byte
      case 5: bad_event.type = static_cast<MidiChannelEventType>(0x90); break;
    }
    invalid.tracks[0].channel_events = {bad_event};
    rejected(invalid, "out-of-range channel event field " + std::to_string(field) + " was accepted");
  }
  invalid = fixture(std::numeric_limits<Tick>::max());
  rejected(invalid, "overflow-sized output must fail before attempting allocation");
  invalid = fixture(960);
  rejected(invalid, "zero sample rate was accepted", 0.0);
  rejected(invalid, "sample rate rounding to zero was accepted", 0.1);
  rejected(invalid, "NaN sample rate was accepted", std::numeric_limits<double>::quiet_NaN());
  rejected(invalid, "negative tail was accepted", kSampleRate, -0.1);
  rejected(invalid, "infinite tail was accepted", kSampleRate, std::numeric_limits<double>::infinity());
  rejected(invalid, "overflow-sized tail must fail before allocation", kSampleRate, 1.0e300);

  // A finite, plausible MIDI timestamp must not allocate beyond a host's
  // chosen memory budget. The exact boundary succeeds and one fewer fails.
  const auto expected_frames = render(invalid).frameCount();
  require(daw::renderMidiFile(invalid, kSampleRate, 0.1, nullptr, expected_frames).frameCount() == expected_frames,
          "an exact output frame budget was rejected");
  report = sentinel;
  bool limited = false;
  try {
    (void)daw::renderMidiFile(invalid, kSampleRate, 0.1, &report, expected_frames - 1);
  } catch (const std::length_error&) { limited = true; }
  require(limited && report.interpreted_channel_events == sentinel.interpreted_channel_events &&
              report.clipped_samples == sentinel.clipped_samples,
          "output budget must fail before allocation without changing diagnostics");
}

}  // namespace

int main() {
  try {
    releaseAndSustain();
    volumeAndExpression();
    channelsAcrossTracks();
    pitchBend();
    channelModeMessages();
    sameTickOrdering();
    tempoAndDiagnostics();
    overlappingSamePitchNotes();
    distinctTicksAtOneFrame();
    diagnosticsAndValidation();
    std::cout << "classical-daw MIDI render controller tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
