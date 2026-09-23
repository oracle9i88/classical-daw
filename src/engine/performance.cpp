#include "daw/performance.hpp"
#include "daw/score_midi.hpp"
#include "daw/project.hpp"
#include "daw/frozen_track.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace daw {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
constexpr std::size_t max_frames = 48000U*60*30;
std::string read(const std::filesystem::path& path, std::size_t limit) {
  require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)), "document dependency must be a regular file");
  std::ifstream in(path, std::ios::binary | std::ios::ate); const auto n = in.tellg();
  require(bool(in) && n >= 0 && static_cast<std::uint64_t>(n) <= limit, "document dependency exceeds bound or cannot be read");
  std::string bytes(static_cast<std::size_t>(n), '\0'); in.seekg(0); in.read(bytes.data(), n);
  require(bool(in) && in.peek() == std::char_traits<char>::eof() && !in.bad(), "document read failed or file changed"); return bytes;
}
void write(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close();
  require(bool(out), "document write failed");
}
std::string binding(const std::string& score, const std::vector<std::uint8_t>& state) {
  return frozenTrackIdentity(score, "performance-document-v1", "pianoteq", state);
}
std::optional<NotePerformance> lookup(const Performance& take, std::uint64_t id) {
  const auto found = std::find_if(take.notes.begin(), take.notes.end(), [&](const auto& n) { return n.note_id == id; });
  return found == take.notes.end() ? std::nullopt : std::optional<NotePerformance>(*found);
}
void update(Performance& take, std::uint64_t id, const std::optional<NotePerformance>& value) {
  auto found = std::find_if(take.notes.begin(), take.notes.end(), [&](const auto& n) { return n.note_id == id; });
  if (found != take.notes.end()) { if (value) *found = *value; else take.notes.erase(found); }
  else if (value) take.notes.push_back(*value);
}
}
Performance makePerformance(const Score& score, const std::string& name) {
  validateNoteIds(score); require(score.next_note_id != 0, "assign notation IDs first");
  MidiFile midi; std::string error;
  if (!scoreToMidiFile(score,&midi,&error)) throw std::invalid_argument(error);
  Performance result; result.name = name;
  for (const auto& track : midi.tracks) for (const auto& note : track.notes)
    result.mapping.push_back({result.next_note_id++,note.source_notation_ids});
  return result;
}
MidiSampleSequence compilePerformance(const Score& score, const Performance& performance) {
  validateNoteIds(score);
  require(score.next_note_id && score.parts.size() == 1, "performance audition requires one identified piano part");
  require(performance.notes.size() <= 4096, "too many performance overrides");
  MidiFile midi; std::string error;
  if (!scoreToMidiFile(score, &midi, &error)) throw std::invalid_argument(error);
  require(midi.tracks[0].notes.size() <= 4096, "audition supports at most 4096 attacks");
  Tick end = 0;
  for (const auto& m : score.parts[0].measures) end = std::max(end, m.start+m.duration);
  auto sequence = makeMidiSampleSequence(midi, 48000, 5, max_frames, end);
  // Index notation anchors once. Validating each correspondence by scanning
  // every attack would make a single performance edit quadratic in score size.
  std::map<std::uint64_t, const MidiNote*> source_notes;
  for (const auto& note : midi.tracks[0].notes)
    source_notes.emplace(note.source_note_id, &note);
  std::map<std::uint64_t,std::uint64_t> anchors;
  std::map<std::uint64_t,bool> mapped;
  require(performance.mapping.size() == midi.tracks[0].notes.size(),"performance mapping does not cover score attacks");
  for (const auto& map : performance.mapping) {
    require(map.id && map.id < performance.next_note_id && !map.notation_ids.empty() && !mapped[map.id],"invalid performed-note identity");
    mapped[map.id] = true;
    const auto found = source_notes.find(map.notation_ids.front());
    require(found != source_notes.end() && found->second->source_notation_ids == map.notation_ids &&
        anchors.emplace(found->first,map.id).second,"unsupported or stale notation/performance mapping");
  }
  struct Timing { std::size_t on, off; int velocity; };
  std::map<std::uint64_t, Timing> times;
  for (const auto& note : midi.tracks[0].notes) {
    const auto on = static_cast<std::size_t>(std::llround(midi.tempo.tickToSeconds(note.start)*48000));
    const auto off = static_cast<std::size_t>(std::llround(midi.tempo.tickToSeconds(note.end())*48000));
    times.emplace(anchors.at(note.source_note_id), Timing{on, off, note.velocity});
  }
  std::map<std::uint64_t, bool> seen;
  for (const auto& n : performance.notes) {
    require(n.note_id && !seen[n.note_id], "duplicate performance note ID"); seen[n.note_id] = true;
    auto found = times.find(n.note_id);
    require(found != times.end(), "performance target must be an attack, not a rest or tie continuation");
    require(std::isfinite(n.onset_seconds) && std::abs(n.onset_seconds) <= 10 &&
        std::isfinite(n.duration_scale) && n.duration_scale >= .1 && n.duration_scale <= 4 &&
        (n.velocity == -1 || (n.velocity >= 1 && n.velocity <= 127)), "invalid performance edit");
    auto& timing = found->second;
    const auto on = static_cast<long long>(timing.on) + std::llround(n.onset_seconds*48000);
    const auto duration = std::llround(static_cast<double>(timing.off-timing.on)*n.duration_scale);
    require(on >= 0 && duration > 0 && on+duration < static_cast<long long>(max_frames-240000), "performance timing outside audition budget");
    timing.on = static_cast<std::size_t>(on); timing.off = static_cast<std::size_t>(on+duration);
    if (n.velocity != -1) timing.velocity = n.velocity;
    sequence.end_frame = std::max(sequence.end_frame, timing.off);
  }
  // MIDI 1.0 cannot address overlapping voices on the same channel/pitch.
  // Reject ambiguous retriggers (including coincident end/attack) in this slice.
  std::map<std::pair<int,int>, std::vector<Timing>> voices;
  for (const auto& note : midi.tracks[0].notes) voices[{note.channel,note.pitch}].push_back(times.at(anchors.at(note.source_note_id)));
  for (auto& [key, notes] : voices) {
    (void)key; std::sort(notes.begin(), notes.end(), [](auto a, auto b) { return a.on < b.on; });
    for (std::size_t i = 1; i < notes.size(); ++i) require(notes[i].on > notes[i-1].off, "ambiguous same-pitch retrigger in performance");
  }
  // The last used-channel resets are appended by makeMidiSampleSequence.
  for (auto& event : sequence.events) {
    if (event.note_id) {
      event.note_id = anchors.at(event.note_id);
      const auto& time = times.at(event.note_id);
      const bool on = (event.status & 0xf0) == 0x90;
      event.frame = on ? time.on : time.off;
      if (on) event.data2 = static_cast<std::uint8_t>(time.velocity);
    } else if (event.terminal_reset) event.frame = sequence.end_frame;
  }
  require(performance.curves.size() <= 32,"too many control curves");
  std::map<std::uint64_t,bool> curve_ids;
  std::map<std::pair<int,int>,bool> lanes;
  std::vector<TimedMidiEvent> curve_events;
  for (const auto& curve : performance.curves) {
    require(curve.id && !curve_ids[curve.id] && curve.channel < 16 && (curve.controller==11 || curve.controller==64),"invalid control curve");
    curve_ids[curve.id]=true;
    const auto lane=std::make_pair(curve.channel,curve.controller);
    require(!lanes[lane] && curve.points.size() >= 2 && curve.points.size() <= 4096,"duplicate lane or invalid curve size");lanes[lane]=true;
    std::map<std::uint64_t,bool> point_ids;double previous=-1;
    for(const auto& point:curve.points) {
      require(point.id && !point_ids[point.id] && std::isfinite(point.seconds) && point.seconds>=0 && point.seconds>previous &&
          point.seconds*48000 <= static_cast<double>(sequence.end_frame) && std::isfinite(point.value) && point.value>=0 && point.value<=127,"invalid curve point");
      point_ids[point.id]=true;previous=point.seconds;
    }
    require(curve.points.front().seconds==0,"control curve must initialize at time zero");
    // A curve owns this controller lane. Keep synthesized final release resets.
    sequence.events.erase(std::remove_if(sequence.events.begin(),sequence.events.end(),[&](const auto& e){
      return e.status==(0xb0|curve.channel) && e.data1==curve.controller && !e.terminal_reset;
    }),sequence.events.end());
    std::size_t segment=0;int last=-1;
    const auto last_frame=static_cast<std::size_t>(std::llround(curve.points.back().seconds*48000));
    for(std::size_t frame=0;frame<=last_frame;frame=std::min(frame+480,last_frame)) {
      const double seconds=static_cast<double>(frame)/48000;
      while(segment+1<curve.points.size()-1 && curve.points[segment+1].seconds<seconds)++segment;
      const auto& a=curve.points[segment];const auto& b=curve.points[segment+1];
      const double f=std::clamp((seconds-a.seconds)/(b.seconds-a.seconds),0.,1.);
      const int value=static_cast<int>(std::lround(a.value+(b.value-a.value)*f));
      if(value!=last)curve_events.push_back({frame,static_cast<std::uint8_t>(0xb0|curve.channel),curve.controller,static_cast<std::uint8_t>(value),0});
      last=value;if(frame==last_frame)break;
    }
  }
  sequence.frames = sequence.end_frame+240000;
  // Explicit performance lanes initialize before same-frame source events.
  // Other source events retain their original same-frame ordering, and final
  // synthetic pedal resets win over a curve endpoint at the release boundary.
  curve_events.insert(curve_events.end(),sequence.events.begin(),sequence.events.end());
  sequence.events.swap(curve_events);
  std::stable_sort(sequence.events.begin(), sequence.events.end(), [](const auto& a, const auto& b) { return a.frame<b.frame; });
  for(std::size_t i=4096;i<sequence.events.size();++i)
    require(sequence.events[i].frame-sequence.events[i-4096].frame>=256,"too many MIDI events in one realtime slice");
  return sequence;
}
void validatePerformanceDocument(const PerformanceDocument& d) {
  require(!d.performances.empty() && d.performances.size() <= 16 && d.active < d.performances.size(), "invalid performance selection");
  require(!d.piano_state.empty() && d.piano_state.size() <= 16U*1024*1024, "saved piano state required");
  require(std::isfinite(d.gain_db) && d.gain_db >= -60 && d.gain_db <= 0,"invalid audition gain");
  for (const auto& take : d.performances) {
    require(!take.name.empty() && take.name.size() <= 256, "invalid performance name");
    (void)compilePerformance(d.score, take);
  }
}
void savePerformanceDocument(const PerformanceDocument& d, const std::string& directory) {
  validatePerformanceDocument(d);
  namespace fs = std::filesystem;
  const fs::path root(directory);
  require(!root.empty() && !fs::exists(fs::symlink_status(root)), "document directory must be new");
  require(fs::create_directory(root), "cannot create document directory");
  try {
    std::string error;
    if (!writeProjectFile(d.score, (root/"score.dawproj").string(), &error)) throw std::runtime_error(error);
    write(root/"piano.aupreset", {d.piano_state.begin(), d.piano_state.end()});
    const auto score_bytes = read(root/"score.dawproj", 64U*1024*1024);
    std::ostringstream out; out << std::setprecision(17) << "CLASSICAL_DAW_PERFORMANCE 1\n" << std::quoted(binding(score_bytes,d.piano_state)) << '\n';
    out << d.active << ' ' << d.performances.size() << ' ' << d.gain_db << '\n';
    for (const auto& take : d.performances) {
      out << std::quoted(take.name) << ' ' << take.next_note_id << ' ' << take.mapping.size() << '\n';
      for(const auto& map:take.mapping) {out<<map.id<<' '<<map.notation_ids.size();for(auto id:map.notation_ids)out<<' '<<id;out<<'\n';}
      out<<take.curves.size()<<'\n';
      for(const auto& curve:take.curves) {
        out<<curve.id<<' '<<static_cast<int>(curve.channel)<<' '<<static_cast<int>(curve.controller)<<' '<<curve.points.size()<<'\n';
        for(const auto& point:curve.points)out<<point.id<<' '<<point.seconds<<' '<<point.value<<'\n';
      }
      out<<take.notes.size()<<'\n';
      for (const auto& n : take.notes) out << n.note_id << ' ' << n.onset_seconds << ' ' << n.duration_scale << ' ' << n.velocity << '\n';
    }
    out << "end\n";
    // Entry is last. Incomplete/crashed saves cannot be opened as a document.
    write(root/"performances.dawperformance", out.str());
  } catch (...) {
    std::error_code ec;
    for (const auto* name : {"score.dawproj.tmp", "score.dawproj", "piano.aupreset", "performances.dawperformance"}) fs::remove(root/name,ec);
    fs::remove(root,ec); throw;
  }
}
PerformanceDocument loadPerformanceDocument(const std::string& directory) {
  namespace fs = std::filesystem; const fs::path root(directory);
  PerformanceDocument d; std::string error;
  const auto score_bytes = read(root/"score.dawproj",64U*1024*1024);
  if (!readProjectFile((root/"score.dawproj").string(),&d.score,&error)) throw std::runtime_error(error);
  const auto state = read(root/"piano.aupreset",16U*1024*1024); d.piano_state.assign(state.begin(),state.end());
  std::istringstream in(read(root/"performances.dawperformance",170U*1024*1024));
  std::string magic, identity; int version = 0; std::size_t count = 0;
  require(bool(in >> magic >> version) && magic == "CLASSICAL_DAW_PERFORMANCE" && version == 1,"invalid performance document");
  require(bool(in >> std::quoted(identity)) && identity == binding(score_bytes,d.piano_state),"performance score/state binding mismatch");
  require(bool(in >> d.active >> count >> d.gain_db) && count >= 1 && count <= 16 && d.active < count,"invalid performance count");
  for (std::size_t i = 0; i < count; ++i) {
    Performance take; std::size_t notes = 0;
    std::size_t mappings=0,curves=0;
    require(bool(in>>std::quoted(take.name)>>take.next_note_id>>mappings) && mappings<=4096,"invalid performance entry");
    for(std::size_t m=0;m<mappings;++m){PerformedNoteMapping map;std::size_t segments=0;
      require(bool(in>>map.id>>segments) && segments>0 && segments<=4096,"invalid note mapping");
      for(std::size_t j=0;j<segments;++j){std::uint64_t id=0;require(bool(in>>id),"truncated mapping");map.notation_ids.push_back(id);}take.mapping.push_back(std::move(map));
    }
    require(bool(in>>curves) && curves<=32,"invalid curve count");
    for(std::size_t c=0;c<curves;++c){ControlCurve curve;int channel=0,cc=0;std::size_t points=0;
      require(bool(in>>curve.id>>channel>>cc>>points) && channel>=0 && channel<16 && cc>=0 && cc<128 && points<=4096,"invalid curve");
      curve.channel=static_cast<std::uint8_t>(channel);curve.controller=static_cast<std::uint8_t>(cc);
      for(std::size_t j=0;j<points;++j){CurvePoint point;require(bool(in>>point.id>>point.seconds>>point.value),"truncated curve");curve.points.push_back(point);}take.curves.push_back(std::move(curve));
    }
    require(bool(in>>notes) && notes<=4096,"invalid override count");
    for (std::size_t j = 0; j < notes; ++j) {
      NotePerformance n;
      require(bool(in >> n.note_id >> n.onset_seconds >> n.duration_scale >> n.velocity),"invalid performance override");
      take.notes.push_back(n);
    }
    d.performances.push_back(std::move(take));
  }
  require(bool(in >> magic) && magic == "end", "truncated performance document");
  in >> std::ws; require(in.eof(),"trailing performance data"); validatePerformanceDocument(d); return d;
}
WorkEditor::WorkEditor(PerformanceDocument d) : document_(std::move(d)) {
  validatePerformanceDocument(document_); history_.reserve(129);
}
void WorkEditor::select(std::size_t take) {
  require(take < document_.performances.size(),"unknown performance");
  if (take == document_.active) return;
  const auto previous = document_.active; document_.active = take;
  try { if (admission_) admission_(document_,revision_+1); }
  catch (...) { document_.active = previous; throw; }
  ++revision_;
}
void WorkEditor::apply(const Change& c, bool forward) {
  const auto prior_active = document_.active;
  auto admit = [&] {
    document_.active = c.take;
    try { if (admission_) admission_(document_,revision_+1); }
    catch (...) { document_.active = prior_active; throw; }
  };
  if(c.kind==Kind::Pitch) {
    ScoreNote* found=nullptr;
    for(auto& p:document_.score.parts)for(auto& m:p.measures)for(auto& n:m.notes)if(n.id==c.id)found=&n;
    require(found,"unknown notation element");const auto previous=found->pitch;
    found->pitch=forward?c.pitch_after:c.pitch_before;
    try{validatePerformanceDocument(document_);admit();}catch(...){found->pitch=previous;throw;}
  } else if(c.kind==Kind::Gain) {
    const auto value=forward?c.value_after:c.value_before;
    require(std::isfinite(value)&&value>=-60&&value<=0,"invalid gain");
    const auto previous = document_.gain_db; document_.gain_db=value;
    try { admit(); } catch (...) { document_.gain_db = previous; throw; }
  } else {
    auto take=document_.performances.at(c.take);
    if(c.kind==Kind::Note)update(take,c.id,forward?c.after:c.before);
    else {
      CurvePoint* found=nullptr;
      for(auto& curve:take.curves)if(curve.id==c.id)for(auto& point:curve.points)if(point.id==c.point)found=&point;
      require(found,"unknown curve/point");found->value=forward?c.value_after:c.value_before;
    }
    (void)compilePerformance(document_.score,take);std::swap(document_.performances[c.take],take);
    try { admit(); } catch (...) { std::swap(document_.performances[c.take],take); throw; }
  }
}
bool WorkEditor::commit(Change c) {
  // Fixed-size delta commands and preallocated history storage: after apply
  // succeeds, history mutation cannot allocate or fail. No snapshot stack copy.
  static_assert(std::is_nothrow_copy_constructible_v<Change>);
  apply(c,true);history_.resize(cursor_);if(history_.size()==128)history_.erase(history_.begin());
  history_.push_back(c);cursor_=history_.size();++revision_;return true;
}
bool WorkEditor::set(const NotePerformance& value) {
  const auto prior=lookup(document_.performances[document_.active],value.note_id);
  if(prior&&prior->onset_seconds==value.onset_seconds&&prior->duration_scale==value.duration_scale&&prior->velocity==value.velocity)return false;
  Change c;c.take=document_.active;c.id=value.note_id;c.before=prior;c.after=value;return commit(c);
}
bool WorkEditor::setPitch(std::uint64_t id,ScorePitch pitch) {
  Change c;c.kind=Kind::Pitch;c.take=document_.active;c.id=id;c.pitch_after=pitch;
  bool found=false;for(const auto& p:document_.score.parts)for(const auto& m:p.measures)for(const auto& n:m.notes)if(n.id==id){c.pitch_before=n.pitch;found=true;}
  require(found,"unknown notation element");
  if(c.pitch_before.step==pitch.step&&c.pitch_before.alter==pitch.alter&&c.pitch_before.octave==pitch.octave)return false;
  return commit(c);
}
bool WorkEditor::setCurvePoint(std::uint64_t curve,std::uint64_t point,double value) {
  Change c;c.kind=Kind::Curve;c.take=document_.active;c.id=curve;c.point=point;c.value_after=value;
  bool found=false;for(const auto& lane:document_.performances[c.take].curves)if(lane.id==curve)for(const auto& p:lane.points)if(p.id==point){found=true;c.value_before=p.value;}
  require(found,"unknown curve/point");if(c.value_before==value)return false;return commit(c);
}
bool WorkEditor::setGain(double value) {
  if(value==document_.gain_db)return false;Change c;c.kind=Kind::Gain;c.take=document_.active;c.value_before=document_.gain_db;c.value_after=value;return commit(c);
}
bool WorkEditor::undo(){if(!cursor_)return false;apply(history_[cursor_-1],false);--cursor_;++revision_;return true;}
bool WorkEditor::redo(){if(cursor_==history_.size())return false;apply(history_[cursor_],true);++cursor_;++revision_;return true;}
}
