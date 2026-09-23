#include "performance_audition.hpp"
#include "coreaudio_output.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <map>

namespace {
void listNotes(const daw::PerformanceDocument& d, const std::string& label, std::size_t offset, std::size_t count) {
  if(count==0||count>200)throw std::runtime_error("notes count must be 1..200");
  struct Location {const daw::ScorePart* part;const daw::ScoreMeasure* measure;const daw::ScoreNote* note;std::size_t ordinal;};
  std::map<std::uint64_t,Location> locations;
  for(const auto& p:d.score.parts)for(std::size_t i=0;i<p.measures.size();++i)
    for(const auto& n:p.measures[i].notes)locations.emplace(n.id,Location{&p,&p.measures[i],&n,i+1});
  const auto& take=d.performances[d.active];const auto sequence=daw::compilePerformance(d.score,take);
  std::map<std::uint64_t,std::size_t> attacks;
  for(const auto& e:sequence.events)if((e.status&0xf0)==0x90&&e.data2)attacks[e.note_id]=e.frame;
  std::size_t matched=0,shown=0;
  for(const auto& mapping:take.mapping) {
    const auto& at=locations.at(mapping.notation_ids.front());const auto& n=*at.note;const auto& m=*at.measure;
    const auto name=m.label.empty()?std::to_string(m.number):m.label;
    bool in_measure=label.empty();
    for(auto id:mapping.notation_ids){const auto& linked=*locations.at(id).measure;
      if((linked.label.empty()?std::to_string(linked.number):linked.label)==label)in_measure=true;}
    if(!in_measure)continue;
    if(matched++<offset||shown>=count)continue;
    ++shown;std::cout<<"performed="<<mapping.id<<" notation=";for(auto id:mapping.notation_ids)std::cout<<id<<',';
    std::cout<<" part="<<std::quoted(at.part->id)<<" measure="<<std::quoted(name)<<" ordinal="<<at.ordinal
      <<" staff="<<n.staff<<" voice="<<n.voice<<" pitch="<<n.pitch.step<<" alter="<<n.pitch.alter<<" octave="<<n.pitch.octave
      <<" offset_ticks="<<n.start-m.start<<" tick="<<n.start<<" duration_ticks="<<n.duration
      <<" attack_seconds="<<static_cast<double>(attacks.at(mapping.id))/48000<<'\n';
  }
  std::cout<<"shown="<<shown<<" matched="<<matched<<" offset="<<offset<<" next_offset="<<offset+shown<<'\n';
}
}

int main(int argc,char** argv) {
  try {
    if(argc!=2)throw std::runtime_error("usage: daw_performance_play DOCUMENT_DIRECTORY");
    daw::WorkEditor editor(daw::loadPerformanceDocument(argv[1]));
    std::unique_ptr<daw::PerformanceAudition> audition;
    daw::CoreAudioOutput output;std::string error;
    auto stop=[&]{output.stop();output.setAudioSource(nullptr);
      if(audition && audition->suppressedConflictsAfterStop())
        std::cout<<"Held keys preserved; conflicting attacks skipped this pass="<<audition->suppressedConflictsAfterStop()<<'\n';
      audition.reset();};
    auto play=[&]{stop();audition=std::make_unique<daw::PerformanceAudition>(editor.document(),false,false,editor.revision());
      if(!output.setAudioSource(audition.get(),&error)||!output.start(&error)){stop();throw std::runtime_error(error);}
      audition->start();std::cout<<"Playing actual realtime Pianoteq; latency="<<audition->latency()<<"s\n";
    };
    editor.setCommitAdmission([&](const auto& document,auto revision){
      if(output.running() && audition)audition->submit(document,revision);
    });
    std::cout<<"Commands: play | stop | take INDEX | notes [OFFSET COUNT] | notes-at LABEL [OFFSET COUNT] | edit PERFORMED_ID OFFSET_MS SCALE VELOCITY(-1=score) | pitch NOTATION_ID STEP ALTER OCTAVE | curves | curve CURVE_ID POINT_ID VALUE | curve-put ID CHANNEL CC POINT_ID SECONDS VALUE [POINT_ID SECONDS VALUE ...] | curve-remove ID | gain DB | undo | redo | save NEW_DIRECTORY | status | quit\n";
    std::string pending;bool done=false;
    while(!done) {
      if(output.running()&&(!output.checkHealth(&error)||(audition&&audition->failed()))){stop();std::cout<<"Output stopped: "<<error<<'\n';}
      if(audition&&audition->done()){stop();std::cout<<"Playback ended\n";}
      std::cout.flush();pollfd input{STDIN_FILENO,POLLIN,0};const int ready=poll(&input,1,100);
      if(ready<0)throw std::runtime_error("stdin poll failed");if(!ready)continue;
      char data[1024];const auto n=read(STDIN_FILENO,data,sizeof(data));
      if(n<=0){done=true;continue;}pending.append(data,static_cast<std::size_t>(n));
      if(pending.size()>8192)throw std::runtime_error("command too long");
      std::size_t newline=0;
      while((newline=pending.find('\n'))!=std::string::npos) {
        const auto line=pending.substr(0,newline);pending.erase(0,newline+1);
        try {
          std::istringstream in(line);std::string command;in>>command;
          auto end=[&]{in>>std::ws;if(!in.eof())throw std::runtime_error("unexpected command arguments");};
          auto parsed=[&]{if(!in)throw std::runtime_error("invalid command arguments");end();};
          if(command=="quit"){end();done=true;break;}
          if(command=="play"){end();play();continue;}
          if(command=="stop"){end();stop();continue;}
          if(command=="save"){std::string path;in>>std::quoted(path);parsed();daw::savePerformanceDocument(editor.document(),path);std::cout<<"Saved "<<path<<'\n';continue;}
          if(command=="notes"||command=="notes-at") {
            std::string label;std::size_t offset=0,count=50;
            if(command=="notes-at"){in>>std::quoted(label);if(!in||label.empty())throw std::runtime_error("measure label required");}
            in>>std::ws;if(!in.eof()){in>>offset>>count;parsed();}
            listNotes(editor.document(),label,offset,count);continue;
          }
          if(command=="curves") {end();for(const auto& curve:editor.document().performances[editor.document().active].curves){
            std::cout<<"curve="<<curve.id<<" channel="<<static_cast<int>(curve.channel)<<" cc="<<static_cast<int>(curve.controller)<<'\n';
            for(const auto& p:curve.points)std::cout<<" point="<<p.id<<" seconds="<<p.seconds<<" value="<<p.value<<'\n';}continue;}
          if(command=="status"){end();std::cout<<"revision="<<editor.revision()<<" active="<<editor.document().active<<" frame="<<(audition?audition->frame():0)
            <<" applied_revision="<<(audition?audition->appliedRevision():editor.revision())<<" applied_frame="<<(audition?audition->appliedFrame():0)<<'\n';continue;}
          // Admission waits for the callback decision before history acceptance.
          // Rejected/cancelled updates leave BOTH document and playback intact.
          if(command=="edit"){daw::NotePerformance note;in>>note.note_id>>note.onset_seconds>>note.duration_scale>>note.velocity;parsed();note.onset_seconds/=1000;editor.set(note);}
          else if(command=="pitch"){std::uint64_t id;daw::ScorePitch pitch;in>>id>>pitch.step>>pitch.alter>>pitch.octave;parsed();editor.setPitch(id,pitch);}
          else if(command=="curve"){std::uint64_t curve,point;double value;in>>curve>>point>>value;parsed();editor.setCurvePoint(curve,point,value);}
          else if(command=="curve-put") {
            daw::ControlCurve curve;int channel,cc;in>>curve.id>>channel>>cc;
            if(!in||channel<0||channel>15||(cc!=11&&cc!=64))throw std::runtime_error("channel must be 0..15 and CC 11 or 64");
            curve.channel=static_cast<std::uint8_t>(channel);curve.controller=static_cast<std::uint8_t>(cc);
            while(true){in>>std::ws;if(in.eof())break;daw::CurvePoint point;in>>point.id>>point.seconds>>point.value;
              if(!in)throw std::runtime_error("curve point requires ID SECONDS VALUE");curve.points.push_back(point);}
            editor.putCurve(std::move(curve));std::cout<<"Explicit curve overrides imported messages on this CC lane; undo restores them.\n";
          }
          else if(command=="curve-remove"){std::uint64_t id;in>>id;parsed();editor.removeCurve(id);}
          else if(command=="gain"){double value;in>>value;parsed();editor.setGain(value);}
          else if(command=="take"){std::size_t take;in>>take;parsed();editor.select(take);}
          else if(command=="undo"){end();editor.undo();}
          else if(command=="redo"){end();editor.redo();}
          else throw std::runtime_error("unknown command");
          std::cout<<"Accepted revision="<<editor.revision()<<'\n';
        }catch(const std::exception& e){std::cout<<"Command failed: "<<e.what()<<'\n';}
      }
    }
    stop();return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
