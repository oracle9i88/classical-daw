#include "daw/session.hpp"
#include "daw/session_mix.hpp"
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
    daw::SessionMixState mix(session);
    for (int i = 3; i < argc;) {
      const std::string option(argv[i++]);
      if (option == "--master") {
        if (i == argc) throw std::runtime_error("--master requires dB");
        mix.apply({daw::MixParameter::Master, "", number(argv[i++])}); continue;
      }
      if (option != "--mute" && option != "--solo" && option != "--gain" && option != "--balance")
        throw std::runtime_error("unknown edit option");
      if (i + 1 >= argc) throw std::runtime_error("track edit requires part ID and value");
      const std::string part(argv[i++]), value(argv[i++]);
      const auto parameter = option == "--mute" ? daw::MixParameter::Mute : option == "--solo" ?
          daw::MixParameter::Solo : option == "--gain" ? daw::MixParameter::Gain : daw::MixParameter::Balance;
      mix.apply({parameter, part, option == "--mute" || option == "--solo" ? static_cast<double>(flag(value)) : number(value)});
    }
    daw::saveNewSessionMix(mix.current(), input.string(), output.string());
    std::cout << "Saved mix settings: " << output << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Session edit failed: " << error.what() << '\n'; return 1;
  }
}
