#include "daw/performance_recovery.hpp"
#include <iostream>
int main(int argc,char** argv){try{
  if(argc==3&&std::string(argv[1])=="list") {
    for(const auto& path:daw::listPerformanceRecoveries(argv[2]))std::cout<<path<<'\n';
  }else if(argc==5&&std::string(argv[1])=="restore") {
    daw::restorePerformanceRecovery(argv[2],argv[3],argv[4]);std::cout<<"Recovered to "<<argv[4]<<'\n';
  }else throw std::runtime_error("usage: daw_performance_recover list SOURCE | restore SOURCE RECOVERY NEW_DIRECTORY");
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
