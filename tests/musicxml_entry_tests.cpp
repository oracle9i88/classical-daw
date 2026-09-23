#include "daw/score.hpp"
#include "daw/project.hpp"
#include "daw/performance.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
namespace fs=std::filesystem;
void require(bool v,const std::string& why){if(!v)throw std::runtime_error(why);}
int main(){fs::path root=fs::temp_directory_path()/("daw-entry-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
  fs::create_directory(root);
  const std::string head="<score-partwise><part-list><score-part id='P1'><part-name>Piano</part-name></score-part></part-list><part id='P1'>";
  const std::string note="<note><pitch><step>C</step><octave>4</octave></pitch><duration>480</duration></note>";
  const std::string tail="</part></score-partwise>";
  auto read=[&](const std::string& xml,daw::Score& score,std::string& error){std::ofstream out(root/"input.xml");out<<xml;out.close();return daw::readMusicXmlFile((root/"input.xml").string(),&score,&error);};
  std::string error;
  const auto basic=head+"<measure number='1'>"+note+"</measure>"+tail;
  for(const auto& declaration:{std::string("<!DOCTYPE score-partwise>"),std::string("<!DOCTYPE score-partwise SYSTEM 'file:///not-read.dtd'>"),
      std::string("<!DOCTYPE score-partwise PUBLIC '-//Recordare//DTD MusicXML 4.0 Partwise//EN' 'https://invalid.example/not-fetched.dtd'>")}){
    daw::Score score;
    require(read("\xef\xbb\xbf<?xml version='1.0'?>\n<!-- <note>&undeclared;</note> -->"+declaration+basic+"<!-- end -->",score,error),error);
    require(score.parts[0].measures[0].notes.size()==1,"comment injected notes");
  }
  for(const auto& xml:{"<!DOCTYPE score-partwise [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>"+basic,
      "<!DOCTYPE score-partwise [<!ENTITY x '123'>]>"+basic,
      "<!ENTITY x '1'>"+basic,"<!DOCTYPE score-partwise SYSTEM unquoted>"+basic,
      "<!DOCTYPE nope>"+basic,"<!DOCTYPE score-partwise><!DOCTYPE score-partwise>"+basic,
      basic+"<!DOCTYPE score-partwise>","<!-- no end"+basic,"<!-- bad -- comment -->"+basic,
      head+"<![CDATA[<note/>]]>"+tail,head+"&unknown;"+tail}) {
    daw::Score unchanged;unchanged.bpm=77;
    require(!read(xml,unchanged,error),"unsafe/malformed declaration accepted");
    require(unchanged.bpm==77,"failed parse changed caller");
  }
  for(const std::string label:{"0","X1","-1","pickup","01","1.5","9999999999999999999999","A&amp;B","A&amp;lt;B"}){
    daw::Score score;
    require(read(head+"<measure number='"+label+"' implicit='yes'>"+note+"</measure><measure number='X2'>"+std::string("<note><pitch><step>D</step><octave>4</octave></pitch><duration>480</duration></note>")+"</measure>"+tail,score,error),error);
    const auto expected=label=="A&amp;B"?"A&B":label=="A&amp;lt;B"?"A&lt;B":label;
    require(score.parts[0].measures[0].label==expected,"label lost");
    require(score.parts[0].measures[1].start==480,"pickup moved timeline");
    // Includes unidentified v8 with a zero allocator, then identified v8.
    for(int identified=0;identified<2;++identified){
      if(identified)daw::assignNoteIds(score);
      require(daw::writeProjectFile(score,(root/"score").string(),&error),error);
      daw::Score reopened;require(daw::readProjectFile((root/"score").string(),&reopened,&error),error);
      require(reopened.parts[0].measures[0].label==expected&&reopened.next_note_id==score.next_note_id,"v8 lost label or IDs");
      require(daw::writeMusicXmlFile(reopened,(root/"export.xml").string(),&error),error);
      daw::Score reimported;require(daw::readMusicXmlFile((root/"export.xml").string(),&reimported,&error),error);
      require(reimported.parts[0].measures[0].label==expected&&reimported.parts[0].measures[1].start==480,"XML label/extent roundtrip");
    }
    auto performance=daw::makePerformance(score,"Imported");
    require(performance.mapping.size()==2&&daw::compilePerformance(score,performance).events.size()>0,"import-to-performance bridge");
  }
  fs::remove_all(root);std::cout<<"PASS safe ignored DTD/comments, rejected entities, pickup/token labels, v8/XML/identity roundtrips\n";
}catch(const std::exception& e){fs::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}}
