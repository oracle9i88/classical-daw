#include "daw/performance_fixture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv) {
  try {
    if(argc!=3)throw std::runtime_error("usage: daw_performance_fixture PIANO.aupreset NEW_DIRECTORY");
    const auto n=std::filesystem::file_size(argv[1]);if(!n||n>16U*1024*1024)throw std::runtime_error("invalid state size");
    std::ifstream in(argv[1],std::ios::binary);std::vector<std::uint8_t> state{std::istreambuf_iterator<char>(in),{}};
    daw::savePerformanceDocument(daw::performanceFixture(std::move(state)),argv[2]);
    std::cout<<"Saved eight bars, tied notation segments, two performances, CC64 and CC11 curves\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
