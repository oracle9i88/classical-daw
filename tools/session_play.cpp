#include "daw/session_player.hpp"
#include "daw/session_mix.hpp"
#include "daw/session_recovery.hpp"
#include <memory>
#include "daw/frozen_track.hpp"
#include "daw/project.hpp"
#include "audio_unit_instrument.hpp"
#include "coreaudio_output.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
std::string read(const fs::path& path, std::size_t limit) {
  if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("source must be a regular file, not a symlink: " + path.filename().string());
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const auto size = in.tellg();
  if (!in || size < 0 || static_cast<std::uintmax_t>(size) > limit) throw std::runtime_error("source unreadable or exceeds size limit");
  std::string result(static_cast<std::size_t>(size), '\0');
  in.seekg(0); in.read(result.data(), static_cast<std::streamsize>(size));
  if (!in) throw std::runtime_error("source read failed");
  return result;
}
double number(std::istringstream& input) {
  double value = 0;
  if (!(input >> value) || !std::isfinite(value)) throw std::runtime_error("expected finite number");
  return value;
}
void end(std::istringstream& input) {
  input >> std::ws;
  if (!input.eof()) throw std::runtime_error("unexpected extra arguments");
}
void help() {
  std::cout << "play | pause | stop | seek SECONDS | master DB\n"
               "gain PART DB | balance PART -1..1 | mute PART 0|1 | solo PART 0|1\n"
               "undo | redo | mix | save NEW_FILENAME | recovery | status | help | quit\n"
               "Starts paused. Save writes a new sibling session; originals are never overwritten.\n";
}
}
int main(int argc, char** argv) {
  try {
    const std::string option = argc == 3 ? argv[1] : "";
    const bool streaming = option == "--stream" || option == "--stream-check" || option == "--stream-device-check";
    const bool check = option == "--check" || option == "--stream-check";
    const bool device_check = option == "--device-check" || option == "--stream-device-check";
    if (!(argc == 2 && argv[1][0] != '-') && !check && !device_check && !streaming) {
      std::cerr << "Usage: daw_session_play [--check | --device-check | --stream | --stream-check | --stream-device-check] SESSION.dawsession\n";
      return 2;
    }
    const fs::path path(argv[check || device_check || streaming ? 2 : 1]);
    const auto source_bytes = read(path, 1024U * 1024U);
    const auto session = daw::parseSession(source_bytes);
    const auto root = path.parent_path();
    const auto score_bytes = read(root / session.score_file, 64U * 1024U * 1024U);
    daw::Score score;
    std::string error;
    if (!daw::readProjectFile((root / session.score_file).string(), &score, &error)) throw std::runtime_error(error);
    const auto plan = daw::planSession(session, score);
    if (!streaming && plan.frames > daw::SessionPlayer::kMaxAudioBytes / sizeof(float) / 2 / session.routes.size())
      throw std::runtime_error("playback audio exceeds 512 MiB; use --stream");
    std::vector<daw::AudioBuffer> audio;
    std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
    std::size_t state_total = 0;
    for (std::size_t i = 0; i < session.routes.size(); ++i) {
      const auto& r = session.routes[i];
      if (r.state_file.empty() || r.frozen_file.empty()) throw std::runtime_error("every track needs saved state and frozen audio; render the session first");
      const auto bytes = read(root / r.state_file, 16U * 1024U * 1024U);
      state_total += bytes.size();
      if (state_total > 64U * 1024U * 1024U) throw std::runtime_error("session states exceed 64 MiB");
      const std::vector<std::uint8_t> state(bytes.begin(), bytes.end());
      if (r.instrument == "swam-cello") daw::validateInstrumentStatePerformance(
          daw::InstrumentKind::SwamCello3, state, plan.tracks[i].midi);
      const auto identity = daw::frozenTrackIdentity(score_bytes, r.part_id, r.instrument, state);
      if (streaming) readers.push_back(std::make_unique<daw::FrozenTrackReader>((root / r.frozen_file).string(), identity, plan.frames));
      else audio.push_back(std::move(daw::readFrozenTrack((root / r.frozen_file).string(), identity, plan.frames).audio));
    }
    auto stream = streaming ? std::make_unique<daw::StreamingAudio>(std::move(readers)) : nullptr;
    auto* stream_view = stream.get(); // Borrowed until player destruction; same render consumer in --check.
    auto player_owner = streaming ? std::make_unique<daw::SessionPlayer>(std::move(stream), session) :
                                    std::make_unique<daw::SessionPlayer>(session, std::move(audio));
    auto& player = *player_owner;
    daw::SessionMixState mix(session);
    if (check) {
      player.enqueue({daw::PlaybackAction::Play});
      std::array<float, 512> block{};
      double peak = 0, energy = 0;
      const std::size_t total = player.frameCount() + daw::SessionPlayer::kRampFrames;
      for (std::size_t frame = 0; frame < total;) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(256, total - frame));
        // Offline check runs faster than wall clock. Wait only on this control
        // thread, before render, so compare identical samples without starvation.
        if (stream_view && frame < player.frameCount()) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          while (!stream_view->frame(frame)) {
            if (stream_view->failed() || std::chrono::steady_clock::now() > deadline)
              throw std::runtime_error("stream prefetch failed or timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        player.render(block.data(), n);
        peak = std::max(peak, player.status().block_peak);
        for (std::size_t j = 0; j < n * 2; ++j) energy += static_cast<double>(block[j]) * block[j];
        frame += n;
      }
      const auto status = player.status();
      std::cout << std::setprecision(12) << "{\"tracks\":" << player.trackCount()
                << ",\"frames\":" << player.frameCount() << ",\"final_frame\":" << status.frame
                << ",\"peak\":" << peak << ",\"rms\":" << std::sqrt(energy / static_cast<double>(total * 2))
                << ",\"clipped_samples\":" << status.clipped_samples
                << ",\"playing\":" << (status.playing ? "true" : "false") << "}\n";
      return status.clipped_samples ? 1 : 0;
    }
    daw::CoreAudioOutput output;
    if (!output.setAudioSource(&player, &error) || !output.start(&error)) throw std::runtime_error(error);
    if (device_check) {
      // Deliberately paused: exercise real output lifecycle without sounding notes.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline && output.renderedFrames() < 4800 && !output.xrunCount()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .02, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::cout << output.diagnostics() << '\n';
      output.stop();
      if (output.xrunCount()) throw std::runtime_error("output callback errors during device check");
      if (!output.renderedFrames()) throw std::runtime_error("output produced no callbacks during device check");
      std::cout << "Output started/stopped with paused playback; " << output.renderedFrames()
                << " silent frames, no instrument plugins loaded.\n";
      return 0;
    }
    if (stream_view) std::cout << "Streaming with " << stream_view->bufferBytes()
                               << " bytes of audio page buffers; all frozen files validated.\n";
    std::cout << "Loaded " << player.trackCount() << " independent tracks, "
              << static_cast<double>(player.frameCount()) / 48000 << " seconds.\n";
    for (const auto& r : session.routes) std::cout << "  " << std::quoted(r.part_id) << ": " << r.instrument
        << " gain=" << r.gain_db << " balance=" << r.balance << " mute=" << r.mute << " solo=" << r.solo << '\n';
    try {
      const auto candidates = daw::listSessionMixRecoveries(path.string());
      if (!candidates.empty()) std::cout << "Recovery candidates found: " << candidates.size()
          << "; inspect with daw_session_recover --list " << path << '\n';
    } catch (const std::exception& e) { std::cerr << "Recovery scan failed: " << e.what() << '\n'; }
    std::unique_ptr<daw::SessionMixRecovery> recovery;
    // Only the control thread touches disk. An already accepted audible edit
    // stays applied on I/O failure; show the lag and retry on the next edit.
    auto checkpoint = [&] {
      if (!mix.revision() || (recovery && recovery->savedRevision() == mix.revision())) return;
      try {
        if (!recovery) recovery = std::make_unique<daw::SessionMixRecovery>(path.string(), source_bytes, score_bytes);
        recovery->checkpoint(mix.current(), mix.revision());
        std::cout << "Recovery checkpoint revision=" << recovery->savedRevision()
                  << " directory=" << std::quoted(recovery->directory()) << std::endl;
      } catch (const std::exception& e) {
        std::cerr << "Mix applied, but recovery NOT saved at revision=" << mix.revision()
                  << ": " << e.what() << "; use save or retry recovery\n";
      }
    };
    help();
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
      try {
        std::istringstream input(line); input.imbue(std::locale::classic());
        std::string word; input >> word;
        if (word.empty()) continue;
        if (word == "quit") { end(input); break; }
        if (word == "help") { end(input); help(); continue; }
        if (word == "recovery") {
          end(input); checkpoint();
          std::cout << "Recovery saved revision=" << (recovery ? recovery->savedRevision() : 0)
                    << ", current revision=" << mix.revision() << '\n';
          continue;
        }
        if (word == "undo" || word == "redo") {
          end(input);
          if (word == "undo" ? !mix.canUndo() : !mix.canRedo())
            throw std::runtime_error(word == "undo" ? "no mix edit to undo" : "no mix edit to redo");
          if (!(word == "undo" ? mix.undo(&player) : mix.redo(&player)))
            throw std::runtime_error("command queue full; history and mix unchanged");
          checkpoint();
          std::cout << (word == "undo" ? "Undid" : "Redid") << " mix edit; revision=" << mix.revision() << '\n';
          continue;
        }
        if (word == "mix") {
          end(input);
          std::cout << daw::serializeSession(mix.current());
          continue;
        }
        if (word == "save") {
          std::string filename;
          if (!(input >> std::quoted(filename)) || filename.empty()) throw std::runtime_error("save requires a new filename");
          end(input);
          const fs::path name(filename);
          if (name.has_parent_path() || name.filename() != name || filename == "." || filename == "..")
            throw std::runtime_error("save requires a sibling filename, not a path");
          daw::saveNewSessionMix(mix.current(), path.string(), (root / name).string());
          std::cout << "Saved accepted mix settings: " << (root / name) << '\n';
          continue;
        }
        if (word == "status") {
          end(input); const auto s = player.status();
          std::cout << (s.playing ? "Playing " : "Paused ") << static_cast<double>(s.frame) / 48000
                    << "s, last-block peak=" << s.block_peak << ", clipped samples=" << s.clipped_samples
                    << ", rejected commands=" << s.rejected_commands << ", callback errors=" << output.xrunCount()
                    << ", buffering frames=" << s.buffering_frames << ", buffering=" << s.buffering
                    << ", stream failed=" << s.stream_failed
                    << ", recovery saved revision=" << (recovery ? recovery->savedRevision() : 0)
                    << ", mix revision=" << mix.revision() << ", undo=" << mix.canUndo() << ", redo=" << mix.canRedo() << '\n';
          continue;
        }
        daw::PlaybackCommand command;
        if (word == "play") command.action = daw::PlaybackAction::Play;
        else if (word == "pause") command.action = daw::PlaybackAction::Pause;
        else if (word == "stop") command.action = daw::PlaybackAction::Stop;
        else if (word == "seek") {
          command.action = daw::PlaybackAction::Seek;
          const double seconds = number(input);
          if (seconds < 0 || seconds > static_cast<double>(player.frameCount()) / 48000) throw std::runtime_error("seek outside session");
          command.frame = static_cast<std::uint64_t>(std::llround(seconds * 48000));
        } else if (word == "master" || word == "gain" || word == "balance" || word == "mute" || word == "solo") {
          std::string part;
          if (word != "master") input >> std::quoted(part);
          const double value = number(input);
          end(input);
          const auto parameter = word == "master" ? daw::MixParameter::Master : word == "gain" ?
              daw::MixParameter::Gain : word == "balance" ? daw::MixParameter::Balance :
              word == "mute" ? daw::MixParameter::Mute : daw::MixParameter::Solo;
          if (!mix.apply({parameter, part, value}, &player)) throw std::runtime_error("command queue full; mix edit not applied or saved");
          checkpoint();
          continue;
        } else throw std::runtime_error("unknown command; enter help");
        end(input);
        if (!player.enqueue(command)) throw std::runtime_error("value out of range or command queue full; command not applied");
      } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    }
    output.stop();
    if (player.status().clipped_samples) std::cerr << "Playback exceeded unity; lower track/master gain. Safety clamp affected monitoring only.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Session playback failed: " << error.what() << '\n'; return 1;
  }
}
