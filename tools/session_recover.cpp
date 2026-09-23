#include "daw/session_recovery.hpp"
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--list") {
      for (const auto& path : daw::listSessionMixRecoveries(argv[2])) {
        std::cout << std::quoted(path);
        try { std::cout << " revision=" << daw::readSessionMixRecovery(argv[2], path).revision << " valid\n"; }
        catch (const std::exception& error) { std::cout << " unavailable: " << error.what() << '\n'; }
      }
      return 0;
    }
    if (argc != 4) {
      std::cerr << "Usage: daw_session_recover --list SOURCE\n"
                   "       daw_session_recover SOURCE RECOVERY_DIRECTORY NEW_SIBLING_SESSION\n";
      return 2;
    }
    daw::recoverSessionMix(argv[1], argv[2], argv[3]);
    std::cout << "Recovered mix to new session: " << argv[3] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Mix recovery failed: " << error.what() << '\n'; return 1;
  }
}
