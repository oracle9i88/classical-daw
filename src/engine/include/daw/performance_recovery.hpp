#pragma once
#include "daw/performance.hpp"

namespace daw {
// Control thread only. Never modifies the source document. Complete snapshots
// are published by a renamed pointer file; retain two completed revisions.
// Protects against process interruption, not a promise of power-loss durability.
class PerformanceRecovery {
 public:
  explicit PerformanceRecovery(const std::string& source_directory);
  ~PerformanceRecovery();
  PerformanceRecovery(const PerformanceRecovery&)=delete;
  PerformanceRecovery& operator=(const PerformanceRecovery&)=delete;
  void checkpoint(const PerformanceDocument& document, std::uint64_t revision);
  const std::string& directory() const noexcept { return directory_; }
  std::uint64_t savedRevision() const noexcept { return saved_revision_; }
 private:
  std::string source_, baseline_, directory_;
  std::uint64_t saved_revision_ = 0, previous_revision_ = 0;
};
std::vector<std::string> listPerformanceRecoveries(const std::string& source_directory);
PerformanceDocument readPerformanceRecovery(const std::string& source_directory,
                                             const std::string& recovery_directory);
void restorePerformanceRecovery(const std::string& source_directory,
                                const std::string& recovery_directory,
                                const std::string& new_directory);
}
