#pragma once

namespace toposim {

class BmpLogViewer {
public:
  static bool startDetached();
  static void stopAndJoin();
  static bool isRunning();
};

} // namespace toposim
