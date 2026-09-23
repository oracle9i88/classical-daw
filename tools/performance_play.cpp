#include "performance_audition.hpp"
#include "coreaudio_output.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <poll.h>
#include <unistd.h>

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
    std::cout<<"Commands: play | stop | take INDEX | notes | edit PERFORMED_ID OFFSET_MS SCALE VELOCITY(-1=score) | pitch NOTATION_ID STEP ALTER OCTAVE | curve CURVE_ID POINT_ID VALUE | gain DB | undo | redo | save NEW_DIRECTORY | status | quit\n";
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
          if(command=="notes"){end();for(const auto& map:editor.document().performances[editor.document().active].mapping){std::cout<<"performed="<<map.id<<" notation=";for(auto id:map.notation_ids)std::cout<<id<<',';std::cout<<'\n';}continue;}
          if(command=="status"){end();std::cout<<"revision="<<editor.revision()<<" active="<<editor.document().active<<" frame="<<(audition?audition->frame():0)
            <<" applied_revision="<<(audition?audition->appliedRevision():editor.revision())<<" applied_frame="<<(audition?audition->appliedFrame():0)<<'\n';continue;}
          // Admission prepares/publishes a plan before history acceptance. Full
          // mailbox or invalid live updates leave BOTH document and playback intact.
          if(command=="edit"){daw::NotePerformance note;in>>note.note_id>>note.onset_seconds>>note.duration_scale>>note.velocity;parsed();note.onset_seconds/=1000;editor.set(note);}
          else if(command=="pitch"){std::uint64_t id;daw::ScorePitch pitch;in>>id>>pitch.step>>pitch.alter>>pitch.octave;parsed();editor.setPitch(id,pitch);}
          else if(command=="curve"){std::uint64_t curve,point;double value;in>>curve>>point>>value;parsed();editor.setCurvePoint(curve,point,value);}
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
