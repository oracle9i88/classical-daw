#include "daw/session_bundle.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: daw_session_collect SAVED_SESSION NEW_DIRECTORY\n"
                   "       daw_session_collect --check SAVED_SESSION\n";
      return 2;
    }
    const bool check = std::string(argv[1]) == "--check";
    const auto report = check ? daw::verifySessionBundle(argv[2]) : daw::collectSessionBundle(argv[1], argv[2]);
    std::cout << (check ? "Validated" : "Collected") << " frozen session: tracks=" << report.tracks
              << " frames=" << report.frames << " referenced_bytes=" << report.referenced_bytes
              << "; no plugins loaded. Plugin state compatibility is not certified.\n";
    if (!check) std::cout << "Open the new directory's session.dawsession; source untouched.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "Session collection failed: " << e.what() << '\n'; return 1; }
}
