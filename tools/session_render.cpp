#include "audio_unit_instrument.hpp"
#include "daw/session.hpp"
#include "daw/project.hpp"
#include "daw/frozen_track.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
std::string read(const fs::path& path, std::size_t limit) {
  if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("input must be a regular file, not a symlink: " + path.filename().string());
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto count = input.tellg();
  if (!input || count < 0 || static_cast<std::uintmax_t>(count) > limit) throw std::runtime_error("input cannot be read or exceeds size limit");
  std::string result(static_cast<std::size_t>(count), '\0');
  input.seekg(0);
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (!input) throw std::runtime_error("input read failed");
  return result;
}
void write(const fs::path& path, const void* bytes, std::size_t size) {
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("cannot create bundle file");
  output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  output.close();
  if (!output) throw std::runtime_error("bundle write failed");
}
std::string quote(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
    else out << static_cast<char>(c);
  }
  out << '"';
  return out.str();
}
struct Bundle {
  fs::path directory;
  std::vector<fs::path> owned;
  bool created = false, complete = false;
  fs::path path(const std::string& name) {
    owned.push_back(directory / name);
    return owned.back();
  }
  ~Bundle() {
    if (!created || complete) return;
    std::error_code ignored;
    for (const auto& path : owned) fs::remove(path, ignored);
    fs::remove(directory, ignored);
  }
};
struct Meter {
  double peak = 0, energy = 0;
  std::uint64_t samples = 0;
  void add(const daw::AudioBuffer& audio) {
    for (float value : audio.samples) {
      peak = std::max(peak, std::abs(static_cast<double>(value)));
      energy += static_cast<double>(value) * value;
    }
    samples += audio.samples.size();
  }
  daw::MixReport report() const { return {peak, samples ? std::sqrt(energy / static_cast<double>(samples)) : 0, 0}; }
};
struct InstrumentState {
  daw::InstrumentKind kind;
  std::string name;
  std::uint32_t version;
  std::vector<std::uint8_t> bytes;
};
}

int main(int argc, char** argv) {
  try {
    const std::string option = argc > 1 ? argv[1] : "";
    const bool incremental_check = argc == 4 && option == "--incremental-check";
    const bool incremental = incremental_check || (argc == 5 && option == "--incremental");
    const bool check_only = incremental_check || (argc == 3 && option == "--check");
    const bool stream = incremental || (argc == 4 && (option == "--stream" || option == "--stream-frozen"));
    const bool frozen = argc == 4 && (std::string(argv[1]) == "--frozen" || std::string(argv[1]) == "--stream-frozen");
    if (!check_only && !frozen && !stream && (argc != 3 || option.compare(0, 2, "--") == 0)) {
      std::cerr << "Usage: daw_session_render INPUT.dawsession NEW_OUTPUT_DIRECTORY\n"
                   "       daw_session_render --check INPUT.dawsession\n"
                   "       daw_session_render --frozen INPUT.dawsession NEW_OUTPUT_DIRECTORY\n"
                   "       daw_session_render --stream INPUT.dawsession NEW_OUTPUT_DIRECTORY\n"
                   "       daw_session_render --stream-frozen INPUT.dawsession NEW_OUTPUT_DIRECTORY\n"
                   "       daw_session_render --incremental PREVIOUS.dawsession CURRENT.dawsession NEW_OUTPUT_DIRECTORY\n"
                   "       daw_session_render --incremental-check PREVIOUS.dawsession CURRENT.dawsession\n";
      return 2;
    }
    const fs::path input = incremental ? argv[3] : (check_only || frozen || stream) ? argv[2] : argv[1];
    const auto source = daw::parseSession(read(input, 1024U * 1024U));
    const auto score_path = input.parent_path() / source.score_file;
    if (!fs::is_regular_file(fs::symlink_status(score_path))) throw std::runtime_error("score must be a regular sibling file");
    const auto score_bytes = read(score_path, 64U * 1024U * 1024U);
    daw::Score score;
    std::string error;
    if (!daw::readProjectFile(score_path.string(), &score, &error)) throw std::runtime_error("score load: " + error);
    const auto plan = daw::planSession(source, score, 48000, 5, stream ? daw::SessionPlanMode::Streaming : daw::SessionPlanMode::Buffered);
    const auto audible = daw::audibleSessionRoutes(source);
    std::vector<std::vector<std::uint8_t>> source_states;
    std::size_t state_bytes = 0;
    for (const auto& route : source.routes) {
      if (frozen && (route.frozen_file.empty() || route.state_file.empty()))
        throw std::runtime_error("frozen reuse requires saved audio and state on every route; render once first");
      std::vector<std::uint8_t> bytes;
      if (!route.state_file.empty()) {
        const auto raw = read(input.parent_path() / route.state_file, 16U * 1024U * 1024U);
        if (raw.empty()) throw std::runtime_error("instrument state is empty");
        state_bytes += raw.size();
        if (state_bytes > 64U * 1024U * 1024U) throw std::runtime_error("session instrument states exceed 64 MiB");
        bytes.assign(raw.begin(), raw.end());
        if (route.instrument == "swam-cello") daw::validateInstrumentStatePerformance(
            daw::InstrumentKind::SwamCello3, bytes, plan.tracks[source_states.size()].midi);
      }
      source_states.push_back(std::move(bytes));
    }
    // A previous cache is never trusted solely because its filename or MIDI
    // looks right. Compare stable part/state/performance, then validate its
    // original score-bound identity and complete CRC before any plugin loads.
    std::vector<std::unique_ptr<daw::FrozenTrackReader>> reused(plan.tracks.size());
    if (incremental) {
      const fs::path old_input(argv[2]);
      const auto old_session = daw::parseSession(read(old_input, 1024U * 1024U));
      const auto old_score_path = old_input.parent_path() / old_session.score_file;
      const auto old_score_bytes = read(old_score_path, 64U * 1024U * 1024U);
      daw::Score old_score;
      if (!daw::readProjectFile(old_score_path.string(), &old_score, &error)) throw std::runtime_error("previous score load: " + error);
      const auto old_plan = daw::planSession(old_session, old_score, 48000, 5, daw::SessionPlanMode::Streaming);
      std::size_t old_state_bytes = 0, reused_count = 0;
      for (std::size_t i = 0; i < plan.tracks.size(); ++i) {
        const auto& route = plan.tracks[i].route;
        const auto found = std::find_if(old_session.routes.begin(), old_session.routes.end(),
            [&](const auto& old) { return old.part_id == route.part_id; });
        std::string reason;
        if (found == old_session.routes.end()) reason = "new part";
        else if (found->instrument != route.instrument) reason = "instrument changed";
        else if (found->state_file.empty() || source_states[i].empty()) reason = "saved state required";
        else {
          const auto raw = read(old_input.parent_path() / found->state_file, 16U * 1024U * 1024U);
          old_state_bytes += raw.size();
          if (old_state_bytes > 64U * 1024U * 1024U) throw std::runtime_error("previous instrument states exceed 64 MiB");
          const std::vector<std::uint8_t> old_state(raw.begin(), raw.end());
          if (old_state != source_states[i]) reason = "instrument state changed";
          else if (!daw::sameSessionPerformance(old_plan, static_cast<std::size_t>(found - old_session.routes.begin()), plan, i))
            reason = "MIDI performance or shared timing changed";
          else if (found->frozen_file.empty()) reason = "no previous frozen audio";
          else {
            const auto identity = daw::frozenTrackIdentity(old_score_bytes, found->part_id, found->instrument, old_state);
            reused[i] = std::make_unique<daw::FrozenTrackReader>(
                (old_input.parent_path() / found->frozen_file).string(), identity, old_plan.frames);
            ++reused_count;
          }
        }
        std::cerr << (reused[i] ? "REUSE " : "RENDER ") << std::quoted(route.part_id)
                  << ": " << (reused[i] ? "validated unchanged performance/state" : reason) << '\n';
      }
      if (incremental_check)
        std::cout << "Incremental plan: reuse=" << reused_count << " render=" << plan.tracks.size()-reused_count << ". ";
    }
    if (check_only) {
      std::cout << "Routing valid: " << plan.tracks.size() << " instruments; " << plan.frames
                << " frames at 48000 Hz. Plugins were not loaded or validated.\n";
      return 0;
    }
    Bundle bundle{fs::path(argv[incremental ? 4 : frozen || stream ? 3 : 2]), {}};
    std::error_code ec;
    const auto status = fs::symlink_status(bundle.directory, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) || fs::exists(status)) throw std::runtime_error("output directory must be new");

    // Validate every instrument/preset/state before creating output. Buffered
    // mode keeps one stem and master; streaming mode keeps fixed audio chunks.
    std::vector<InstrumentState> states;
    state_bytes = 0;
    for (std::size_t i = 0; i < plan.tracks.size(); ++i) {
      const auto& route = plan.tracks[i].route;
      const auto kind = route.instrument == "pianoteq" ? daw::InstrumentKind::Pianoteq9 : daw::InstrumentKind::SwamCello3;
      if (frozen || reused[i]) {
        state_bytes += source_states[i].size();
        if (state_bytes > 64U * 1024U * 1024U) throw std::runtime_error("session instrument states exceed 64 MiB");
        states.push_back({kind, "", 0, std::move(source_states[i])});
        continue;  // No AU discovery, instantiation or licensing on frozen reuse.
      }
      daw::AudioUnitInstrument setup(kind);
      auto bytes = std::move(source_states[i]);
      if (!bytes.empty()) setup.restoreState(bytes);
      else {
        setup.selectFactoryPreset(route.preset.empty() ? setup.descriptor().default_preset : route.preset);
        bytes = setup.state();
      }
      state_bytes += bytes.size();
      daw::validateInstrumentStatePerformance(kind, bytes, plan.tracks[i].midi);
      if (state_bytes > 64U * 1024U * 1024U) throw std::runtime_error("session instrument states exceed 64 MiB");
      states.push_back({kind, setup.presetName(), setup.componentVersion(), std::move(bytes)});
    }
    daw::AudioBuffer mix;
    mix.channels = 2;
    if (!stream) mix.samples.assign(plan.frames * 2, 0.0F);
    daw::Session saved = source;
    saved.score_file = "score.dawproj";
    std::ostringstream report;
    report.imbue(std::locale::classic());
    report << std::setprecision(17) << "{\n  \"format\": \"classical-daw-session-bounce-2\","
           << "\n  \"sample_rate\": 48000,\n  \"channels\": 2,\n  \"frames\": " << plan.frames
           << ",\n  \"end_tick\": " << plan.end_tick
           << ",\n  \"master_gain_db\": " << source.master_gain_db
           << ",\n  \"tail_seconds\": 5,\n  \"stems\": [\n";
    if (!fs::create_directory(bundle.directory)) throw std::runtime_error("output directory must be new and creatable");
    bundle.created = true;
    for (std::size_t i = 0; i < plan.tracks.size(); ++i) {
      const auto& routed = plan.tracks[i];
      const bool reuse_audio = frozen || bool(reused[i]);
      auto& state = states[i];
      daw::InstrumentRenderReport diagnostics;
      const auto identity = daw::frozenTrackIdentity(score_bytes, routed.route.part_id, routed.route.instrument, state.bytes);
      const auto base = "track-" + std::to_string(i + 1);
      const auto frozen_name = base + ".dawfreeze";
      const auto wav_name = base + ".wav", midi_name = base + ".mid", state_name = base + ".aupreset";
      daw::AudioBuffer stem;
      daw::MixReport stem_meter;
      if (stream) {
        auto cache = std::move(reused[i]);
        if (reuse_audio) {
          if (!cache) cache = std::make_unique<daw::FrozenTrackReader>((input.parent_path() / routed.route.frozen_file).string(), identity, plan.frames);
          state.name = cache->presetName(); state.version = cache->componentVersion();
        }
        daw::FrozenTrackWriter frozen_writer(bundle.path(frozen_name).string(), identity, state.name, state.version, plan.frames);
        daw::WavPcm16Writer wav_writer(bundle.path(wav_name).string(), plan.frames);
        stem.channels = 2; stem.samples.reserve(8192 * 2);
        Meter meter;
        std::size_t received = 0;
        auto consume = [&](std::size_t frame, const float* samples, std::uint32_t count) {
          if (frame != received || count > plan.frames - received) throw std::runtime_error("streamed stem timing mismatch");
          frozen_writer.appendFrames(samples, count);
          stem.samples.assign(samples, samples + count * 2);
          if (audible[i]) daw::applyTrackMix(stem, routed.route.gain_db, routed.route.balance);
          else std::fill(stem.samples.begin(), stem.samples.end(), 0.0F);
          meter.add(stem);
          if (meter.peak > 1) throw std::runtime_error("stem exceeds PCM16 headroom; lower that track's gain");
          wav_writer.appendFrames(stem.samples.data(), count); // Rejects any PCM16 overload before publication.
          received += count;
        };
        if (reuse_audio) {
          std::cerr << "Streaming frozen audio: " << routed.route.part_id << '\n';
          std::vector<float> block(8192 * 2);
          for (std::size_t frame = 0; frame < plan.frames; frame += 8192) {
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(8192, plan.frames - frame));
            cache->readFrames(frame, count, block.data()); consume(frame, block.data(), count);
          }
        } else {
          daw::AudioUnitInstrument instrument(state.kind);
          instrument.restoreState(state.bytes);
          if (instrument.presetName() != state.name) throw std::runtime_error("restoring the instrument changed its preset");
          if (state.kind == daw::InstrumentKind::Pianoteq9 && instrument.state() != state.bytes) throw std::runtime_error("piano state changed on restoration");
          std::cerr << "Streaming instrument: " << routed.route.part_id << '\n';
          instrument.renderChunks(routed.midi, consume, &diagnostics, 5, plan.end_tick);
          if (diagnostics.peak == 0 && !routed.midi.tracks.front().notes.empty()) throw std::runtime_error("instrument returned silence for sounding notes");
        }
        frozen_writer.finish(); wav_writer.finish(); stem_meter = meter.report();
      } else {
        if (frozen) {
          std::cerr << "Reusing frozen audio: " << routed.route.part_id << '\n';
          auto cached = daw::readFrozenTrack((input.parent_path() / routed.route.frozen_file).string(), identity, plan.frames);
          stem = std::move(cached.audio); state.name = std::move(cached.preset); state.version = cached.component_version;
        } else {
          daw::AudioUnitInstrument instrument(state.kind);
          instrument.restoreState(state.bytes);
          if (instrument.presetName() != state.name) throw std::runtime_error("restoring the instrument changed its preset");
          if (state.kind == daw::InstrumentKind::Pianoteq9 && instrument.state() != state.bytes) throw std::runtime_error("piano state changed on restoration");
          std::cerr << "Rendering " << routed.route.part_id << ": " << instrument.descriptor().name << '\n';
          stem = instrument.render(routed.midi, &diagnostics, 48000, 5, false, plan.end_tick);
          if (diagnostics.peak == 0 && !routed.midi.tracks.front().notes.empty()) throw std::runtime_error("instrument returned silence for sounding notes");
        }
        if (stem.frameCount() != plan.frames) throw std::runtime_error("rendered stem length does not match the session clock");
        daw::writeFrozenTrack(stem, identity, state.name, state.version, bundle.path(frozen_name).string());
        if (audible[i]) daw::applyTrackMix(stem, routed.route.gain_db, routed.route.balance);
        else std::fill(stem.samples.begin(), stem.samples.end(), 0.0F);
        stem_meter = daw::applyMasterMix(stem, 0);
        if (stem_meter.over_unity_samples != 0) throw std::runtime_error("stem exceeds PCM16 headroom; lower that track's gain");
        daw::addStereoTrack(mix, stem);
        if (!daw::writeWavPcm16(stem, bundle.path(wav_name).string(), &error)) throw std::runtime_error(error);
      }
      saved.routes[i].frozen_file = frozen_name;
      bundle.path(midi_name + ".tmp");
      if (!daw::writeMidiFile(routed.midi, bundle.path(midi_name).string(), &error)) throw std::runtime_error(error);
      write(bundle.path(state_name), state.bytes.data(), state.bytes.size());
      saved.routes[i].preset.clear();
      saved.routes[i].state_file = state_name;
      if (i) report << ",\n";
      report << "    {\"part_id\": " << quote(routed.route.part_id)
             << ", \"instrument\": " << quote(routed.route.instrument) << ", \"preset\": " << quote(state.name)
             << ", \"component_version\": " << state.version << ", \"audio\": " << quote(wav_name)
             << ", \"midi\": " << quote(midi_name) << ", \"state\": " << quote(state_name)
             << ", \"gain_db\": " << routed.route.gain_db << ", \"balance\": " << routed.route.balance
             << ", \"mute\": " << (routed.route.mute ? "true" : "false")
             << ", \"solo\": " << (routed.route.solo ? "true" : "false")
             << ", \"audible\": " << (audible[i] ? "true" : "false")
             << ", \"frozen\": " << quote(frozen_name) << ", \"audio_source\": " << quote(reuse_audio ? "frozen" : "plugin")
             << ", \"peak\": " << stem_meter.peak << ", \"rms\": " << stem_meter.rms
             << ", \"sent_messages\": " << diagnostics.sent_messages
             << ", \"skipped_bank_program_messages\": " << diagnostics.skipped_instrument_selection
             << ", \"plugin_over_unity_samples\": " << (reuse_audio ? "null" : std::to_string(diagnostics.over_unity_samples)) << '}';
    }
    daw::MixReport master;
    if (stream) {
      std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
      for (std::size_t i = 0; i < saved.routes.size(); ++i) {
        const auto& route = saved.routes[i];
        const auto identity = daw::frozenTrackIdentity(score_bytes, route.part_id, route.instrument, states[i].bytes);
        readers.push_back(std::make_unique<daw::FrozenTrackReader>((bundle.directory / route.frozen_file).string(), identity, plan.frames));
      }
      daw::WavPcm16Writer master_writer(bundle.path("mix.wav").string(), plan.frames);
      daw::AudioBuffer block{48000, 2, {}}, track{48000, 2, {}};
      block.samples.reserve(8192 * 2); track.samples.reserve(8192 * 2);
      Meter meter;
      for (std::size_t frame = 0; frame < plan.frames; frame += 8192) {
        const auto count = std::min<std::size_t>(8192, plan.frames - frame);
        block.samples.assign(count * 2, 0); track.samples.resize(count * 2);
        for (std::size_t i = 0; i < readers.size(); ++i) {
          if (!audible[i]) continue;
          readers[i]->readFrames(frame, count, track.samples.data());
          daw::applyTrackMix(track, saved.routes[i].gain_db, saved.routes[i].balance);
          daw::addStereoTrack(block, track);
        }
        const auto report = daw::applyMasterMix(block, source.master_gain_db);
        if (report.over_unity_samples) throw std::runtime_error("master exceeds PCM16 headroom; lower the master gain");
        meter.add(block); master_writer.appendFrames(block.samples.data(), count);
      }
      master_writer.finish(); master = meter.report();
    } else master = daw::applyMasterMix(mix, source.master_gain_db);
    if (master.over_unity_samples != 0) throw std::runtime_error("master exceeds PCM16 headroom; lower the master gain");
    report << "\n  ],\n  \"master_peak\": " << master.peak << ",\n  \"master_rms\": " << master.rms
           << ",\n  \"clipped_samples\": 0,\n  \"stem_tap\": \"post-track-fader-and-mute-solo, pre-master\"\n}\n";
    // Preserve exact source bytes so the frozen identity remains valid on reopen.
    write(bundle.path(saved.score_file), score_bytes.data(), score_bytes.size());
    if (!stream && !daw::writeWavPcm16(mix, bundle.path("mix.wav").string(), &error)) throw std::runtime_error(error);
    const auto json = report.str();
    write(bundle.path("report.json"), json.data(), json.size());
    const auto session = daw::serializeSession(saved);
    // The session entry point is written last: a process crash cannot leave a
    // complete-looking saved session that references unfinished stems/state.
    write(bundle.path("session.dawsession"), session.data(), session.size());
    bundle.complete = true;
    std::cout << "Wrote " << plan.tracks.size() << " aligned stems, mix.wav and reloadable session: " << bundle.directory << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Session render failed: " << error.what() << '\n';
    return 1;
  }
}
