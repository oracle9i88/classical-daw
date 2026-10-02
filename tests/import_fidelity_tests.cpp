#include "daw/performance_fixture.hpp"
#include "daw/project.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool value, const std::string& why) {
  if (!value) throw std::runtime_error(why);
}
std::string note(char step, int duration, bool chord = false) {
  return std::string("<note>") + (chord ? "<chord/>" : "") +
      "<pitch><step>" + step + "</step><octave>4</octave></pitch><duration>" +
      std::to_string(duration) + "</duration></note>";
}
std::string meter(int beats) {
  return "<attributes><divisions>960</divisions><time><beats>" +
      std::to_string(beats) + "</beats><beat-type>4</beat-type></time></attributes>";
}
const std::string grace = "<note><grace/><pitch><step>D</step><octave>4</octave></pitch>"
                          "<type>eighth</type></note>";
int firstControl(const daw::MidiSampleSequence& sequence, int cc) {
  for (const auto& event : sequence.events)
    if (event.frame == 0 && event.status == 0xb0 && event.data1 == cc) return event.data2;
  return -1;
}
int main() {
  const auto root = fs::temp_directory_path() /
      ("daw-fidelity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directory(root);
    std::string error;
    // A grace decorating a PRINCIPAL chord must not extend the bar. Include
    // an unequal short tone, graces arriving across a barline, and another
    // voice whose duration must not be affected by the borrowing.
    for (const int chord_length : {3840, 240}) {
      for (const bool cross_bar : {false, true}) {
        const auto path = root / "grace.xml";
        std::ofstream out(path);
        out << "<score-partwise><part-list><score-part id='P1'><part-name>Piano</part-name>"
               "</score-part></part-list><part id='P1'>";
        if (cross_bar)
          out << "<measure number='0'>" << meter(4) << note('A',3840) << grace << "</measure>";
        out << "<measure number='1'>" << meter(4) << (cross_bar ? "" : grace)
            << note('C',3840) << note('E',chord_length,true) << note('G',3840,true)
            << "<backup><duration>3840</duration></backup>"
               "<note><pitch><step>B</step><octave>3</octave></pitch><duration>3840</duration>"
               "<voice>2</voice></note></measure><measure number='2'>"
            << note('F',3840) << note('A',3840,true) << "</measure></part></score-partwise>";
        out.close();
        daw::Score score; daw::MusicXmlImportReport report;
        require(daw::readMusicXmlFile(path.string(), &score, &error, &report), error);
        const std::size_t bar = cross_bar ? 1 : 0;
        const auto start = static_cast<daw::Tick>(bar * 3840);
        const auto borrowed = chord_length == 240 ? 120 : 480;
        const auto& notes = score.parts[0].measures[bar].notes;
        require(notes.size() == 5, "grace/principal/other voice missing");
        require(notes[0].duration == borrowed && notes[0].start == start, "wrong grace loan");
        for (std::size_t i = 1; i <= 3; ++i) {
          require(notes[i].start == start + borrowed, "principal chord attacks separated");
          require(notes[i].start + notes[i].duration == start + (i == 2 ? chord_length : 3840),
                  "principal chord release moved");
        }
        require(notes[4].start == start && notes[4].duration == 3840, "other voice moved");
        const auto& following = score.parts[0].measures[bar + 1];
        require(following.start == start + 3840, "grace chord extended the bar");
        require(following.notes[1].duration == 3840, "grace borrowing leaked into next chord");
        require(daw::validateScore(score, &error), error);
        daw::assignNoteIds(score);
        (void)daw::compilePerformance(score, daw::makePerformance(score,"chord"));
      }
    }

    // Time-zero initialization is musical data, including a lane with no
    // later change. Check CC64 and CC11, history, and stored/reopened output.
    for (const int cc : {64, 11}) {
      for (const bool constant : {false, true}) {
        auto d = daw::performanceFixture({1});
        const auto initial = static_cast<std::uint8_t>(cc == 64 ? 127 : 32);
        auto& events = d.score.parts[0].midi_events;
        events = {{0,daw::MidiChannelEventType::ControlChange,0,static_cast<std::uint8_t>(cc),initial,0}};
        if (!constant) {
          events.push_back({960,daw::MidiChannelEventType::ControlChange,0,static_cast<std::uint8_t>(cc),0,0});
          events.push_back({1920,daw::MidiChannelEventType::ControlChange,0,static_cast<std::uint8_t>(cc),127,0});
        }
        d.performances = {daw::makePerformance(d.score,"imported")}; d.active = 0;
        daw::WorkEditor edit(d);
        daw::CurveAdoptionReport report;
        edit.putCurve(daw::curveFromScoreMessages(d.score,0,static_cast<std::uint8_t>(cc),1,&report));
        require(report.source_messages == events.size(), "adoption count differs");
        const auto check = [&](const daw::PerformanceDocument& document) {
          const auto sequence = daw::compilePerformance(document.score,document.performances[0]);
          require(firstControl(sequence,cc) == initial, "time-zero controller lost");
          if (!constant) {
            bool released = false;
            for (const auto& e : sequence.events)
              if (e.frame == 24000 && e.status == 0xb0 && e.data1 == cc && e.data2 == 0) released = true;
            require(released, "later controller release lost");
          }
        };
        check(edit.document());
        require(edit.undo(), "adoption undo missing"); check(edit.document());
        require(edit.redo(), "adoption redo missing"); check(edit.document());
        const auto saved = root / (std::to_string(cc) + (constant ? "-constant" : "-changing"));
        daw::savePerformanceDocument(edit.document(), saved.string());
        check(daw::loadPerformanceDocument(saved.string()));
      }
    }

    // Even a lane that only states the default at zero is valid and editable.
    {
      auto d = daw::performanceFixture({1});
      d.score.parts[0].midi_events = {{0,daw::MidiChannelEventType::ControlChange,0,64,0,0}};
      const auto lane = daw::curveFromScoreMessages(d.score,0,64,1);
      require(lane.stepped && lane.points.size() == 1 && lane.points.front().value == 0 && lane.points.back().value == 0,
              "constant default lane refused or changed");
    }

    // Pick a shorter part with a different initial meter AND a later change.
    // Preserve both through project and XML persistence, plus pedal routing.
    const auto path = root / "meters.xml";
    std::ofstream out(path);
    out << "<score-partwise><part-list><score-part id='P1'><part-name>Upper</part-name></score-part>"
           "<score-part id='P2'><part-name>Lower</part-name></score-part></part-list>"
           "<part id='P1'><measure number='1'>" << meter(4) << note('C',3840)
        << "</measure><measure number='2'>" << note('D',3840) << "</measure></part>"
           "<part id='P2'><measure number='1'>" << meter(3)
        << "<direction><direction-type><pedal type='start'/></direction-type></direction>"
        << note('E',2880) << "</measure><measure number='2'>" << meter(2) << note('F',1920)
        << "</measure></part></score-partwise>";
    out.close();
    daw::Score selected; daw::MusicXmlImportReport report;
    require(daw::readMusicXmlFile(path.string(),&selected,&error,&report,2),error);
    const auto checkMeter = [&](const daw::Score& score) {
      require(score.parts.size() == 1 && score.parts[0].id == "P2", "wrong selected part");
      require(score.time_signature.numerator == 3 && score.time_signature.denominator == 4,
              "selected part inherited another part's initial meter");
      require(score.meter_changes.size() == 1 && score.meter_changes[0].tick == 2880 &&
              score.meter_changes[0].signature.numerator == 2, "selected part lost meter change");
      require(score.parts[0].measures[1].start == 2880, "selected bar start moved");
    };
    checkMeter(selected);
    require(selected.parts[0].midi_events[0].channel == 0, "selected pedal channel lost");
    require(daw::writeProjectFile(selected,(root/"selected.dawproj").string(),&error),error);
    daw::Score reopened;
    require(daw::readProjectFile((root/"selected.dawproj").string(),&reopened,&error),error); checkMeter(reopened);
    require(daw::writeMusicXmlFile(selected,(root/"selected.xml").string(),&error),error);
    require(daw::readMusicXmlFile((root/"selected.xml").string(),&reopened,&error),error); checkMeter(reopened);
    require(!daw::readMusicXmlFile(path.string(),&selected,&error,&report,3),"invalid selection accepted");
    checkMeter(selected); // failed read must leave the caller's score intact
    require(!daw::readMusicXmlFile(path.string(),&reopened,&error),"strict multi-part read changed");
    fs::remove_all(root);
    std::cout << "import musical fidelity tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << " (fixtures: " << root << ")\n";
    return 1;
  }
}
