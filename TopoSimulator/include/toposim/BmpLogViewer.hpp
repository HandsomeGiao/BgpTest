#pragma once

#include <filesystem>

namespace toposim {

class BmpLogViewer {
public:
  static bool startDetached();
  static int runStandalone(const std::filesystem::path &database_file);
  static void stopAndJoin();
  static bool isRunning();
};

} // namespace toposim
