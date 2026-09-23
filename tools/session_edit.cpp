#include "daw/session.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
double number(const std::string& text) {
  std::size_t used = 0;
  const double value = std::stod(text, &used);
  if (used != text.size()) throw std::invalid_argument("invalid number");
  return value;
}
bool flag(const std::string& text) {
  if (text != "0" && text != "1") throw std::invalid_argument("mute/solo must be 0 or 1");
  return text == "1";
}
}
int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      std::cerr << "Usage: daw_session_edit INPUT NEW_SIBLING_SESSION [--mute PART 0|1] [--solo PART 0|1]\n"
                   "       [--gain PART DB] [--balance PART -1..1] [--master DB]\n"
                   "       daw_session_edit --show INPUT\n";
      return 2;
    }
    const bool show = argc == 3 && std::string(argv[1]) == "--show";
    const fs::path input = show ? argv[2] : argv[1];
    if (!fs::is_regular_file(fs::symlink_status(input)) || fs::file_size(input) > 1024U * 1024U)
      throw std::runtime_error("session must be a regular file no larger than 1 MiB");
    std::ifstream file(input, std::ios::binary);
    std::ostringstream text; text << file.rdbuf();
    if (file.bad()) throw std::runtime_error("session read failed");
    auto session = daw::parseSession(text.str());
    if (show) {
      const auto audible = daw::audibleSessionRoutes(session);
      std::cout << "Master: " << session.master_gain_db << " dB\n";
      for (std::size_t i = 0; i < session.routes.size(); ++i) {
        const auto& r = session.routes[i];
        std::cout << std::quoted(r.part_id) << " instrument=" << r.instrument << " gain=" << r.gain_db
                  << " balance=" << r.balance << " mute=" << r.mute << " solo=" << r.solo
                  << " audible=" << audible[i] << " frozen_reference=" << !r.frozen_file.empty() << '\n';
      }
      return 0;
    }
    const fs::path output(argv[2]);
    auto parent = [](const fs::path& p) { return fs::canonical(p.has_parent_path() ? p.parent_path() : fs::path(".")); };
    if (parent(input) != parent(output)) throw std::runtime_error("edited session must stay beside its score/state/audio files");
    if (fs::exists(fs::symlink_status(output))) throw std::runtime_error("output session must be new");
    for (int i = 3; i < argc;) {
      const std::string option(argv[i++]);
      if (option == "--master") {
        if (i == argc) throw std::runtime_error("--master requires dB");
        session.master_gain_db = number(argv[i++]); continue;
      }
      if (option != "--mute" && option != "--solo" && option != "--gain" && option != "--balance")
        throw std::runtime_error("unknown edit option");
      if (i + 1 >= argc) throw std::runtime_error("track edit requires part ID and value");
      const std::string part(argv[i++]), value(argv[i++]);
      auto found = std::find_if(session.routes.begin(), session.routes.end(), [&](const auto& r) { return r.part_id == part; });
      if (found == session.routes.end()) throw std::runtime_error("unknown part ID");
      if (option == "--mute") found->mute = flag(value);
      if (option == "--solo") found->solo = flag(value);
      if (option == "--gain") found->gain_db = number(value);
      if (option == "--balance") found->balance = number(value);
    }
    const auto updated = daw::serializeSession(session);
    std::ofstream saved(output, std::ios::binary);
    saved.write(updated.data(), static_cast<std::streamsize>(updated.size())); saved.close();
    if (!saved) { std::error_code ignored; fs::remove(output, ignored); throw std::runtime_error("session write failed"); }
    std::cout << "Saved mix settings: " << output << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Session edit failed: " << error.what() << '\n'; return 1;
  }
}
