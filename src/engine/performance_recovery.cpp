#include "daw/performance_recovery.hpp"
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace daw {
namespace {
namespace fs = std::filesystem;
void need(bool v, const char* why) { if (!v) throw std::runtime_error(why); }
std::string read(const fs::path& path, std::size_t limit) {
  need(fs::is_regular_file(fs::symlink_status(path)), "recovery file missing or symlink");
  std::ifstream in(path,std::ios::binary|std::ios::ate); const auto n=in.tellg();
  need(bool(in)&&n>=0&&static_cast<std::uint64_t>(n)<=limit,"recovery file exceeds bound");
  std::string bytes(static_cast<std::size_t>(n),'\0');in.seekg(0);in.read(bytes.data(),n);
  need(bool(in)&&in.peek()==std::char_traits<char>::eof(),"recovery read failed");return bytes;
}
void writeNew(const fs::path& path,const std::string& bytes) {
  need(!fs::exists(fs::symlink_status(path)),"recovery temporary already exists");
  std::ofstream out(path,std::ios::binary);out<<bytes;out.close();need(bool(out),"recovery write failed");
}
std::string digest(const fs::path& root) {
  need(fs::is_directory(fs::symlink_status(root)),"document directory missing or symlink");
  const auto score=read(root/"score.dawproj",64U*1024*1024);
  const auto performance=read(root/"performances.dawperformance",170U*1024*1024);
  std::vector<std::string> states;
  std::istringstream header(performance);std::string magic;int version=0;std::size_t routes=0;
  need(bool(header>>magic>>version) && magic=="CLASSICAL_DAW_PERFORMANCE" && version>=1 && version<=4,"invalid recovery document version");
  if(version>=4)need(bool(header>>routes) && routes<=64,"invalid recovery route count");
  std::size_t total=0;
  if(!routes)states.push_back(read(root/"piano.aupreset",16U*1024*1024));
  else for(std::size_t i=0;i<routes;++i){states.push_back(read(root/("route-"+std::to_string(i+1)+".aupreset"),16U*1024*1024));total+=states.back().size();need(total<=256U*1024*1024,"recovery states exceed bound");}
  // Length-delimited FNV-1a detects accidental corruption/stale input. This
  // local recovery journal is not an authenticity check for untrusted files.
  std::uint64_t hash=14695981039346656037ULL;
  const auto byte=[&](unsigned char b){hash^=b;hash*=1099511628211ULL;};
  const auto add=[&](const std::string& bytes){
    const auto size=static_cast<std::uint64_t>(bytes.size());
    for(unsigned shift=0;shift<64;shift+=8)byte(static_cast<unsigned char>(size>>shift));
    for(char b:bytes)byte(static_cast<unsigned char>(b));
  };
  add(score);add(performance);for(const auto& state:states)add(state);
  std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;return out.str();
}
fs::path canonicalSource(const std::string& source) {
  need(fs::is_directory(fs::symlink_status(source)),"source must be a real document directory");
  return fs::canonical(source);
}
std::string prefix(const fs::path& source) { return source.filename().string()+".performance-recovery-"; }
struct Entry { std::uint64_t revision; std::string checksum; };
Entry entry(const fs::path& dir) {
  std::istringstream in(read(dir/"latest",1024));Entry e{};std::string magic;
  need(bool(in>>magic>>e.revision>>e.checksum)&&magic=="DAW_PERFORMANCE_RECOVERY_1"&&e.revision>0,"invalid recovery pointer");
  in>>std::ws;need(in.eof(),"trailing recovery pointer data");return e;
}
void removeSnapshot(const fs::path& dir) noexcept {
  // Only our known files, never recursive deletion of unexpected content.
  std::error_code ec;
  if(!fs::is_directory(fs::symlink_status(dir,ec)))return;
  for(const auto* name:{"score.dawproj","piano.aupreset","performances.dawperformance"})fs::remove(dir/name,ec);
  for(std::size_t i=0;i<64;++i)fs::remove(dir/("route-"+std::to_string(i+1)+".aupreset"),ec);
  fs::remove(dir,ec);
}
}
PerformanceRecovery::PerformanceRecovery(const std::string& source) {
  source_=canonicalSource(source).string(); baseline_=digest(source_);
  std::random_device random;
  for(int i=0;i<8;++i) {
    const auto path=fs::path(source_).parent_path()/(prefix(source_)+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(random()));
    if(fs::create_directory(path)){directory_=path.string();break;}
  }
  need(!directory_.empty(),"cannot reserve recovery directory");
  fs::permissions(directory_,fs::perms::owner_all,fs::perm_options::replace);
  writeNew(fs::path(directory_)/"baseline",baseline_);
}
PerformanceRecovery::~PerformanceRecovery() {
  if(saved_revision_ || directory_.empty())return;
  std::error_code ec;
  if(!fs::is_directory(fs::symlink_status(directory_,ec)))return;
  fs::remove(fs::path(directory_)/"baseline",ec);fs::remove(directory_,ec);
}
void PerformanceRecovery::checkpoint(const PerformanceDocument& document,std::uint64_t revision) {
  need(revision>saved_revision_,"recovery revision must advance");
  need(digest(source_)==baseline_,"source document changed; refusing stale recovery");
  const fs::path dir(directory_);
  need(fs::is_directory(fs::symlink_status(dir)),"recovery directory missing or symlink");
  const auto snapshot=dir/("revision-"+std::to_string(revision));
  const auto staging=dir/".latest.tmp";
  savePerformanceDocument(document,snapshot.string());
  try {
    const auto checksum=digest(snapshot);
    writeNew(staging,"DAW_PERFORMANCE_RECOVERY_1\n"+std::to_string(revision)+"\n"+checksum+"\n");
    need(digest(source_)==baseline_,"source changed during checkpoint");
    fs::rename(staging,dir/"latest");
  } catch(...) {
    std::error_code ec;fs::remove(staging,ec);removeSnapshot(snapshot);throw;
  }
  // Publication succeeded: cleanup is best-effort and cannot revoke this edit.
  if(previous_revision_)removeSnapshot(dir/("revision-"+std::to_string(previous_revision_)));
  previous_revision_=saved_revision_;saved_revision_=revision;
}
std::vector<std::string> listPerformanceRecoveries(const std::string& source) {
  const auto path=canonicalSource(source);std::vector<std::string> result;
  for(const auto& item:fs::directory_iterator(path.parent_path())) {
    if(!fs::is_directory(item.symlink_status())||item.path().filename().string().rfind(prefix(path),0)!=0)continue;
    if(fs::is_regular_file(fs::symlink_status(item.path()/"latest")))result.push_back(item.path().string());
  }
  std::sort(result.begin(),result.end());return result;
}
PerformanceDocument readPerformanceRecovery(const std::string& source,const std::string& directory) {
  const auto path=canonicalSource(source);
  need(fs::is_directory(fs::symlink_status(directory)),"recovery directory missing or symlink");
  const auto dir=fs::canonical(directory);
  need(dir.parent_path()==path.parent_path()&&dir.filename().string().rfind(prefix(path),0)==0,"recovery belongs to a different source");
  need(read(dir/"baseline",1024)==digest(path),"source changed; refusing stale recovery");
  const auto e=entry(dir);const auto snapshot=dir/("revision-"+std::to_string(e.revision));
  need(digest(snapshot)==e.checksum,"recovery checksum mismatch");
  return loadPerformanceDocument(snapshot.string());
}
void restorePerformanceRecovery(const std::string& source,const std::string& recovery,const std::string& destination) {
  savePerformanceDocument(readPerformanceRecovery(source,recovery),destination);
}
}
