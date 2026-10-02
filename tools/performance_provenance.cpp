// Data-only receipt inspection; never loads a plugin or opens an audio device.
#include "daw/performance.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){try {
  if(argc!=2)throw std::invalid_argument("usage: daw_performance_provenance DOCUMENT_DIRECTORY");
  const auto document=daw::loadPerformanceDocument(argv[1]);
  if(document.import_receipt.empty()){
    std::cerr<<"No import receipt in this legacy document; details cannot be reconstructed.\n";return 2;
  }
  std::cout<<document.import_receipt;
  return std::cout?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
