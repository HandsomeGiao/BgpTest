#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <conio.h>
#include <cstdio>
#include <io.h>
#include <windows.h>

#include <spdlog/spdlog.h>

#include "toposim/BmpLogManager.hpp"
#include "toposim/BmpLogViewer.hpp"
#include "toposim/TopoManager.hpp"
#include "toposim/TopologyJson.hpp"

namespace {

bool stdinIsTerminal();
bool stdoutIsTerminal();

enum class BmpViewerMode { Auto, On, Off };

HANDLE consoleOutput() { return GetStdHandle(STD_OUTPUT_HANDLE); }

WORD defaultConsoleAttributes() {
  static const WORD attrs = [] {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(consoleOutput(), &info)) {
      return info.wAttributes;
    }
    return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN |
                             FOREGROUND_BLUE);
  }();
  return attrs;
}

class ScopedConsoleColor {
public:
  explicit ScopedConsoleColor(WORD attrs) : active_(stdoutIsTerminal()) {
    if (active_) {
      SetConsoleTextAttribute(consoleOutput(), attrs);
    }
  }

  ~ScopedConsoleColor() {
    if (active_) {
      SetConsoleTextAttribute(consoleOutput(), defaultConsoleAttributes());
    }
  }

  ScopedConsoleColor(const ScopedConsoleColor &) = delete;
  ScopedConsoleColor &operator=(const ScopedConsoleColor &) = delete;

private:
  bool active_ = false;
};

constexpr WORD kDim = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD kPrompt = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD kCommand = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kArgument = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kInfo = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD kSuccess = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kWarning = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kError = FOREGROUND_RED | FOREGROUND_INTENSITY;
constexpr WORD kCandidate = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;

void writeColored(const std::string &text, WORD attrs) {
  ScopedConsoleColor color(attrs);
  std::cout << text;
}

void writeLineColored(const std::string &text, WORD attrs) {
  writeColored(text, attrs);
  std::cout << '\n';
}

void printPrompt(const std::string &prompt) {
  writeColored(prompt, kPrompt);
}

void printErrorLine(const std::string &message) {
  writeColored("[ERR] ", kError);
  writeLineColored(message, kError);
}

void printInfoLine(const std::string &message) {
  writeColored("[*] ", kInfo);
  std::cout << message << '\n';
}

void printSuccessLine(const std::string &message) {
  writeColored("[OK] ", kSuccess);
  std::cout << message << '\n';
}

void printWarningLine(const std::string &message) {
  writeColored("[!] ", kWarning);
  std::cout << message << '\n';
}

std::vector<std::string> split(const std::string &line) {
  std::istringstream iss(line);
  std::vector<std::string> parts;
  std::string part;
  while (iss >> part) {
    parts.push_back(part);
  }
  return parts;
}

std::vector<std::string> splitForCompletion(const std::string &line) {
  auto parts = split(line);
  if (line.empty() || std::isspace(static_cast<unsigned char>(line.back()))) {
    parts.emplace_back();
  }
  return parts;
}

std::vector<std::string> routerIds(const toposim::TopoManager &manager) {
  std::vector<std::string> ids;
  for (const auto &router : manager.routersSnapshot()) {
    ids.push_back(router.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string commonPrefix(const std::vector<std::string> &values) {
  if (values.empty()) {
    return {};
  }
  auto prefix = values.front();
  for (const auto &value : values) {
    while (!startsWith(value, prefix)) {
      prefix.pop_back();
      if (prefix.empty()) {
        return {};
      }
    }
  }
  return prefix;
}

std::vector<std::string> filteredCandidates(std::vector<std::string> candidates,
                                            const std::string &prefix) {
  std::vector<std::string> matches;
  std::sort(candidates.begin(), candidates.end());
  for (const auto &candidate : candidates) {
    if (startsWith(candidate, prefix)) {
      matches.push_back(candidate);
    }
  }
  return matches;
}

std::vector<std::string>
completionCandidates(const std::string &line,
                     const toposim::TopoManager &manager) {
  const auto parts = splitForCompletion(line);
  const std::size_t index = parts.empty() ? 0 : parts.size() - 1;
  const std::string prefix = parts.empty() ? std::string{} : parts.back();
  const auto routers = routerIds(manager);

  if (index == 0) {
    return filteredCandidates({"advertise", "converge", "exit", "help",
                               "link", "node", "quit", "show", "withdraw"},
                              prefix);
  }

  const auto &command = parts[0];
  if (command == "show") {
    if (index == 1) {
      return filteredCandidates({"peers", "rib", "routers"}, prefix);
    }
    if (index == 2 && parts.size() >= 2 &&
        (parts[1] == "peers" || parts[1] == "rib")) {
      return filteredCandidates(routers, prefix);
    }
  } else if (command == "link") {
    if (index == 1) {
      return filteredCandidates({"down", "up"}, prefix);
    }
    if (index == 2 || index == 3) {
      return filteredCandidates(routers, prefix);
    }
  } else if (command == "node") {
    if (index == 1) {
      return filteredCandidates({"down", "up"}, prefix);
    }
    if (index == 2) {
      return filteredCandidates(routers, prefix);
    }
  } else if ((command == "advertise" || command == "withdraw") && index == 1) {
    return filteredCandidates(routers, prefix);
  }

  return {};
}

void replaceCurrentToken(std::string &line, const std::string &replacement) {
  const auto token_start = line.empty() || std::isspace(static_cast<unsigned char>(line.back()))
                               ? line.size()
                               : line.find_last_of(" \t") == std::string::npos
                                     ? 0
                                     : line.find_last_of(" \t") + 1;
  line.erase(token_start);
  line += replacement;
}

void repaintPrompt(const std::string &prompt, const std::string &line) {
  std::cout << '\r';
  printPrompt(prompt);
  std::cout << line << "  ";
  std::cout << '\r';
  printPrompt(prompt);
  std::cout << line;
  std::cout.flush();
}

std::optional<std::string>
readCommandLine(const std::string &prompt, const toposim::TopoManager &manager) {
  if (!stdinIsTerminal()) {
    std::string line;
    if (std::getline(std::cin, line)) {
      return line;
    }
    return std::nullopt;
  }

  printPrompt(prompt);
  std::cout.flush();
  std::string line;

  while (true) {
    const int ch = _getch();
    if (ch == 3) {
      return std::nullopt;
    }
    if (ch == 13) {
      std::cout << '\n';
      return line;
    }
    if (ch == 8) {
      if (!line.empty()) {
        line.pop_back();
        repaintPrompt(prompt, line);
      }
      continue;
    }
    if (ch == 9) {
      const auto matches = completionCandidates(line, manager);
      if (matches.empty()) {
        std::cout << '\a';
        continue;
      }

      const auto prefix = commonPrefix(matches);
      const auto before = line;
      replaceCurrentToken(line, prefix);
      if (matches.size() == 1) {
        line += ' ';
        repaintPrompt(prompt, line);
      } else {
        if (line != before) {
          repaintPrompt(prompt, line);
        }
        std::cout << '\n';
        for (const auto &match : matches) {
          writeColored("  * ", kDim);
          writeLineColored(match, kCandidate);
        }
        repaintPrompt(prompt, line);
      }
      continue;
    }
    if (ch == 0 || ch == 224) {
      (void)_getch();
      continue;
    }
    if (std::isprint(ch)) {
      line.push_back(static_cast<char>(ch));
      std::cout << static_cast<char>(ch);
      std::cout.flush();
    }
  }
}

void printHelp() {
  writeLineColored("Commands:", kInfo);
  const auto command = [](const std::string &verb, const std::string &args = {}) {
    writeColored("  > ", kDim);
    writeColored(verb, kCommand);
    if (!args.empty()) {
      std::cout << ' ';
      writeColored(args, kArgument);
    }
    std::cout << '\n';
  };
  command("help");
  command("show", "routers");
  command("show", "peers <router>");
  command("show", "rib <router>");
  command("link", "up <a> <b>");
  command("link", "down <a> <b>");
  command("node", "up <router>");
  command("node", "down <router>");
  command("advertise", "<router> <prefix>");
  command("withdraw", "<router> <prefix>");
  command("converge", "[timeout_ms]");
  command("quit");
  writeColored("[TAB] ", kWarning);
  std::cout << "complete commands and router names.\n";
}

bool requireConverged(const toposim::TopoManager &manager) {
  if (manager.isConverged()) {
    return true;
  }
  printWarningLine("Network is not converged. Run 'converge [timeout_ms]' "
                   "before changing links, nodes or routes.");
  return false;
}

bool stdinIsTerminal() {
  return _isatty(_fileno(stdin)) != 0;
}

bool stdoutIsTerminal() {
  return _isatty(_fileno(stdout)) != 0;
}

void waitBeforeExitIfDoubleClicked() {
  if (!stdinIsTerminal()) {
    return;
  }
  std::cout << "\nPress Enter to exit...";
  std::string ignored;
  std::getline(std::cin, ignored);
}

std::vector<std::filesystem::path> discoverTopologies() {
  const auto topo_dir = std::filesystem::current_path() / "topo";
  std::vector<std::filesystem::path> topologies;
  if (!std::filesystem::exists(topo_dir) ||
      !std::filesystem::is_directory(topo_dir)) {
    return topologies;
  }

  for (const auto &entry : std::filesystem::directory_iterator(topo_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() == ".json") {
      topologies.push_back(entry.path());
    }
  }

  std::sort(topologies.begin(), topologies.end());
  return topologies;
}

std::optional<std::filesystem::path> topologyPathFromArgs(int argc,
                                                          char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--topology" || arg == "-t") && i + 1 < argc) {
      return argv[++i];
    }
    if (arg == "--debug") {
      spdlog::set_level(spdlog::level::debug);
    }
  }

  const auto topo_dir = std::filesystem::current_path() / "topo";
  const auto topologies = discoverTopologies();
  if (topologies.empty()) {
    printWarningLine("No available topologies in: " + topo_dir.string());
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  printInfoLine("Available topologies in " + topo_dir.string() + ":");
  for (std::size_t i = 0; i < topologies.size(); ++i) {
    writeColored("  * ", kDim);
    writeColored(std::to_string(i + 1) + ". ", kArgument);
    writeLineColored(topologies[i].filename().string(), kCandidate);
  }

  if (topologies.size() == 1) {
    printSuccessLine("Using topology: " +
                     topologies.front().filename().string());
    return topologies.front();
  }

  if (!stdinIsTerminal()) {
    printWarningLine("Multiple topologies are available. Pass --topology "
                     "<file> in non-interactive mode.");
    return std::nullopt;
  }

  writeColored("Select topology ", kInfo);
  writeColored("[1-" + std::to_string(topologies.size()) + "]: ", kArgument);
  std::string line;
  if (!std::getline(std::cin, line)) {
    return std::nullopt;
  }

  std::size_t selected = 0;
  try {
    selected = static_cast<std::size_t>(std::stoul(line));
  } catch (...) {
    printErrorLine("Invalid selection.");
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  if (selected == 0 || selected > topologies.size()) {
    printErrorLine("Invalid selection.");
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  return topologies[selected - 1];
}

BmpViewerMode bmpViewerModeFromArgs(int argc, char **argv) {
  BmpViewerMode mode = BmpViewerMode::Auto;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--bmp-viewer" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "off") {
        mode = BmpViewerMode::Off;
      } else if (value == "on") {
        mode = BmpViewerMode::On;
      } else if (value == "auto") {
        mode = BmpViewerMode::Auto;
      } else {
        throw std::runtime_error(
            "Invalid --bmp-viewer value. Use auto, on or off.");
      }
    }
  }
  return mode;
}

bool shouldStartBmpViewer(BmpViewerMode mode) {
  if (mode == BmpViewerMode::On) {
    return true;
  }
  if (mode == BmpViewerMode::Off) {
    return false;
  }
  return stdinIsTerminal() && stdoutIsTerminal();
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto bmp_viewer_mode = bmpViewerModeFromArgs(argc, argv);
    const auto topology_path = topologyPathFromArgs(argc, argv);
    if (!topology_path) {
      return 1;
    }
    auto topology = toposim::loadTopologyConfig(*topology_path);
    toposim::TopoManager manager(std::move(topology));

    manager.start();
    if (shouldStartBmpViewer(bmp_viewer_mode)) {
      if (!toposim::BmpLogViewer::startDetached()) {
        printWarningLine("BMP viewer is already running.");
      }
    }
    const bool converged = manager.waitForConvergence(std::chrono::seconds(20));
    if (converged) {
      printSuccessLine("Initial convergence: ok");
    } else {
      printWarningLine("Initial convergence: timeout");
    }
    printInfoLine("BMP collector log: " + manager.logFile().string());
    printInfoLine("BMP SQLite DB: " + manager.databaseFile().string());
    printHelp();

    while (true) {
      auto maybe_line = readCommandLine("bgp> ", manager);
      if (!maybe_line) {
        break;
      }
      const auto &line = *maybe_line;
      const auto parts = split(line);
      if (parts.empty()) {
        continue;
      }
      try {
        if (parts[0] == "quit" || parts[0] == "exit") {
          break;
        }
        if (parts[0] == "help") {
          printHelp();
        } else if (parts.size() == 2 && parts[0] == "show" &&
                   parts[1] == "routers") {
          std::cout << toposim::toJson(manager.routersSnapshot()).dump(2)
                    << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "rib") {
          std::cout << toposim::toJson(manager.ribSnapshot(parts[2])).dump(2)
                    << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "peers") {
          std::cout << toposim::toJson(manager.peersSnapshot(parts[2])).dump(2)
                    << '\n';
        } else if (parts.size() == 4 && parts[0] == "link") {
          if (parts[1] != "up" && parts[1] != "down") {
            printWarningLine("Usage: link <up|down> <a> <b>");
            continue;
          }
          if (!requireConverged(manager)) {
            continue;
          }
          manager.setLinkState(parts[2], parts[3], parts[1] == "up");
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "node") {
          if (parts[1] != "up" && parts[1] != "down") {
            printWarningLine("Usage: node <up|down> <router>");
            continue;
          }
          if (!requireConverged(manager)) {
            continue;
          }
          manager.setRouterState(parts[2], parts[1] == "up");
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "advertise") {
          if (!requireConverged(manager)) {
            continue;
          }
          manager.originatePrefix(parts[1], parts[2]);
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "withdraw") {
          if (!requireConverged(manager)) {
            continue;
          }
          manager.withdrawPrefix(parts[1], parts[2]);
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts[0] == "converge") {
          const auto timeout = parts.size() > 1 ? std::stoi(parts[1]) : 20000;
          if (timeout <= 0) {
            printWarningLine("Usage: converge [positive_timeout_ms]");
            continue;
          }
          const bool ok =
              manager.waitForConvergence(std::chrono::milliseconds(timeout));
          if (ok) {
            printSuccessLine("converged");
          } else {
            printWarningLine("timeout");
          }
        } else {
          printWarningLine("Unknown command. Type 'help'.");
        }
      } catch (const std::exception &ex) {
        printErrorLine(std::string("Command failed: ") + ex.what());
      }
    }

    manager.stop();
    toposim::BmpLogViewer::stopAndJoin();
    toposim::BmpLogManager::instance().shutdown();
    return 0;
  } catch (const std::exception &ex) {
    toposim::BmpLogViewer::stopAndJoin();
    toposim::BmpLogManager::instance().shutdown();
    printErrorLine(std::string("Fatal: ") + ex.what());
    waitBeforeExitIfDoubleClicked();
    return 1;
  }
}
