#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
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
    return filteredCandidates({"advertise", "bmp", "converge", "exit", "help",
                               "link", "node", "quit", "show", "withdraw"},
                              prefix);
  }

  const auto &command = parts[0];
  if (command == "show") {
    if (index == 1) {
      return filteredCandidates({"all-routes", "peers", "rib", "routers"},
                                prefix);
    }
    if (index == 2 && parts.size() >= 2 &&
        (parts[1] == "all-routes" || parts[1] == "peers" ||
         parts[1] == "rib")) {
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
  } else if (command == "bmp" && index == 1) {
    return filteredCandidates({"close", "open", "status", "viewer"}, prefix);
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
    if (std::isprint(static_cast<unsigned char>(ch))) {
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
  command("show", "all-routes <router>");
  command("link", "up <a> <b>");
  command("link", "down <a> <b>");
  command("node", "up <router>");
  command("node", "down <router>");
  command("advertise", "<router> <prefix>");
  command("withdraw", "<router> <prefix>");
  command("converge");
  command("bmp", "viewer|open|close|status");
  command("quit");
  writeColored("[TAB] ", kWarning);
  std::cout << "complete commands and router names.\n";
}

std::string joinAsPath(const std::vector<std::uint32_t> &as_path) {
  if (as_path.empty()) {
    return "-";
  }
  std::ostringstream oss;
  for (std::size_t i = 0; i < as_path.size(); ++i) {
    if (i != 0) {
      oss << ' ';
    }
    oss << as_path[i];
  }
  return oss.str();
}

std::string routeNextHop(const toposim::RouteEntry &route) {
  return route.attributes.next_hop.empty() ? "-" : route.attributes.next_hop;
}

void printAllRoutes(const toposim::TopoManager &manager,
                    const std::string &router_id) {
  const auto rib = manager.ribSnapshot(router_id);
  std::map<std::string, std::vector<std::pair<std::string, toposim::RouteEntry>>>
      paths_by_prefix;
  std::map<std::string, toposim::RouteEntry> best_by_prefix;

  for (const auto &[prefix, route] : rib.local_routes) {
    paths_by_prefix[prefix].push_back({"local", route});
  }
  for (const auto &[peer_id, routes] : rib.adj_rib_in) {
    for (const auto &[prefix, route] : routes) {
      paths_by_prefix[prefix].push_back({"from " + peer_id, route});
    }
  }
  for (const auto &route : rib.loc_rib) {
    best_by_prefix[route.prefix] = route;
    paths_by_prefix.try_emplace(route.prefix);
  }

  if (paths_by_prefix.empty()) {
    printInfoLine("Router " + router_id + " has no known routes.");
    return;
  }

  writeColored("All routes for ", kInfo);
  writeColored(router_id, kCandidate);
  std::cout << ":\n";

  for (auto &[prefix, paths] : paths_by_prefix) {
    std::sort(paths.begin(), paths.end(), [](const auto &lhs, const auto &rhs) {
      return std::tuple{lhs.second.attributes.as_path.size(),
                        lhs.second.attributes.next_hop, lhs.first} <
             std::tuple{rhs.second.attributes.as_path.size(),
                        rhs.second.attributes.next_hop, rhs.first};
    });

    writeColored(prefix, kArgument);
    const auto best_it = best_by_prefix.find(prefix);
    if (best_it == best_by_prefix.end()) {
      std::cout << "  best: -\n";
    } else {
      std::cout << "  best next-hop=" << routeNextHop(best_it->second)
                << " as-path="
                << joinAsPath(best_it->second.attributes.as_path) << '\n';
    }

    for (const auto &[source, route] : paths) {
      const bool is_best =
          best_it != best_by_prefix.end() && route == best_it->second;
      std::cout << "  " << (is_best ? "* " : "  ");
      std::cout << std::left << std::setw(12) << source << std::right
                << " next-hop=" << routeNextHop(route)
                << " as-path=" << joinAsPath(route.attributes.as_path)
                << '\n';
    }
  }
}

std::string formatDuration(std::chrono::steady_clock::duration duration) {
  const auto millis =
      std::max<std::int64_t>(0, std::chrono::duration_cast<
                                    std::chrono::milliseconds>(duration)
                                    .count());
  std::ostringstream oss;
  oss << (millis / 1000) << '.' << std::setw(3) << std::setfill('0')
      << (millis % 1000) << 's';
  return oss.str();
}

std::string convergenceDuration(const toposim::TopoManager &manager,
                                std::chrono::steady_clock::time_point start) {
  const auto last_activity = manager.lastConvergenceActivityAt();
  if (last_activity <= start) {
    return "0.000s";
  }
  return formatDuration(last_activity - start);
}

void updateProgressLine(const std::string &message, bool &printed,
                        std::size_t &previous_width) {
  if (!stdoutIsTerminal()) {
    if (!printed) {
      printInfoLine(message);
      printed = true;
    }
    return;
  }

  std::cout << '\r' << message;
  if (previous_width > message.size()) {
    std::cout << std::string(previous_width - message.size(), ' ');
  }
  std::cout.flush();
  previous_width = (std::max)(previous_width, message.size());
  printed = true;
}

void clearProgressLine(bool printed, std::size_t previous_width) {
  if (!printed || !stdoutIsTerminal()) {
    return;
  }
  std::cout << '\r' << std::string(previous_width, ' ') << '\r';
  std::cout.flush();
}

void waitUntilConverged(const toposim::TopoManager &manager,
                        const std::string &reason,
                        std::chrono::steady_clock::time_point start) {
  auto last_reported = std::uint64_t{0};
  bool progress_printed = false;
  std::size_t progress_width = 0;

  printInfoLine(reason + ": waiting for topology convergence...");
  while (!manager.isConverged()) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (static_cast<std::uint64_t>(elapsed) != last_reported) {
      last_reported = static_cast<std::uint64_t>(elapsed);
      updateProgressLine("Waiting for convergence: " +
                             std::to_string(last_reported) + "s elapsed",
                         progress_printed, progress_width);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  clearProgressLine(progress_printed, progress_width);
  toposim::BmpLogManager::instance().flush();
  printSuccessLine(reason + ": topology converged after " +
                   convergenceDuration(manager, start));
}

bool requireConverged(const toposim::TopoManager &manager) {
  if (manager.isConverged()) {
    return true;
  }
  waitUntilConverged(manager, "Previous operation",
                     std::chrono::steady_clock::now());
  return true;
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

std::filesystem::path executablePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  while (true) {
    const DWORD size = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      throw std::runtime_error("Unable to resolve executable path.");
    }
    if (size < buffer.size()) {
      return std::filesystem::path(buffer.data());
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path executableDirectory() {
  return executablePath().parent_path();
}

std::filesystem::path topologyDirectory() {
  return executableDirectory() / "topo";
}

std::filesystem::path resolveTopologyArgument(std::filesystem::path path) {
  if (path.is_absolute()) {
    return path;
  }
  if (std::filesystem::exists(path)) {
    return path;
  }

  const auto exe_relative = executableDirectory() / path;
  if (std::filesystem::exists(exe_relative)) {
    return exe_relative;
  }

  const auto topo_relative = topologyDirectory() / path;
  if (std::filesystem::exists(topo_relative)) {
    return topo_relative;
  }

  return path;
}

std::vector<std::filesystem::path> discoverTopologies() {
  const auto topo_dir = topologyDirectory();
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
    if (arg == "--topology" || arg == "-t") {
      if (i + 1 >= argc) {
        throw std::runtime_error(arg + " requires a topology file path.");
      }
      return resolveTopologyArgument(argv[++i]);
    }
    if (arg == "--debug") {
      spdlog::set_level(spdlog::level::debug);
    }
  }

  const auto topo_dir = topologyDirectory();
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
    if (arg == "--bmp-viewer") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--bmp-viewer requires auto, on or off.");
      }
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

    const auto startup_started_at = std::chrono::steady_clock::now();
    manager.start();
    if (shouldStartBmpViewer(bmp_viewer_mode)) {
      if (!toposim::BmpLogViewer::startDetached()) {
        printWarningLine("BMP viewer is already running.");
      }
    }
    waitUntilConverged(manager, "Initial convergence", startup_started_at);
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
        } else if (parts[0] == "bmp") {
          if (parts.size() != 2) {
            printWarningLine("Usage: bmp <viewer|open|close|status>");
            continue;
          }
          if (parts[1] == "viewer" || parts[1] == "open") {
            if (toposim::BmpLogViewer::startDetached()) {
              printSuccessLine("BMP viewer opened");
            } else {
              printWarningLine("BMP viewer is already running.");
            }
          } else if (parts[1] == "close") {
            if (toposim::BmpLogViewer::isRunning()) {
              toposim::BmpLogViewer::stopAndJoin();
              printSuccessLine("BMP viewer closed");
            } else {
              printWarningLine("BMP viewer is not running.");
            }
          } else if (parts[1] == "status") {
            if (toposim::BmpLogViewer::isRunning()) {
              printSuccessLine("BMP viewer: running");
            } else {
              printInfoLine("BMP viewer: stopped");
            }
          } else {
            printWarningLine("Usage: bmp <viewer|open|close|status>");
          }
        } else if (parts.size() == 2 && parts[0] == "show" &&
                   parts[1] == "routers") {
          std::cout << toposim::toJson(manager.routersSnapshot()).dump(2)
                    << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "rib") {
          std::cout << toposim::toJson(manager.ribSnapshot(parts[2])).dump(2)
                    << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "all-routes") {
          printAllRoutes(manager, parts[2]);
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
          const auto operation_started_at = std::chrono::steady_clock::now();
          const bool changed =
              manager.setLinkState(parts[2], parts[3], parts[1] == "up");
          if (!changed) {
            printWarningLine(parts[1] == "up" ? "Link is already up."
                                               : "Link is already down.");
            continue;
          }
          waitUntilConverged(manager, "Link state change",
                             operation_started_at);
        } else if (parts.size() == 3 && parts[0] == "node") {
          if (parts[1] != "up" && parts[1] != "down") {
            printWarningLine("Usage: node <up|down> <router>");
            continue;
          }
          if (!requireConverged(manager)) {
            continue;
          }
          const auto operation_started_at = std::chrono::steady_clock::now();
          const bool changed =
              manager.setRouterState(parts[2], parts[1] == "up");
          if (!changed) {
            printWarningLine(parts[1] == "up" ? "Router is already running."
                                               : "Router is already stopped.");
            continue;
          }
          waitUntilConverged(manager, "Router state change",
                             operation_started_at);
        } else if (parts.size() == 3 && parts[0] == "advertise") {
          if (!requireConverged(manager)) {
            continue;
          }
          const auto operation_started_at = std::chrono::steady_clock::now();
          manager.originatePrefix(parts[1], parts[2]);
          waitUntilConverged(manager, "Route advertisement",
                             operation_started_at);
        } else if (parts.size() == 3 && parts[0] == "withdraw") {
          if (!requireConverged(manager)) {
            continue;
          }
          const auto operation_started_at = std::chrono::steady_clock::now();
          manager.withdrawPrefix(parts[1], parts[2]);
          waitUntilConverged(manager, "Route withdrawal",
                             operation_started_at);
        } else if (parts[0] == "converge") {
          if (parts.size() != 1) {
            printWarningLine("Usage: converge");
            continue;
          }
          waitUntilConverged(manager, "Manual convergence wait",
                             std::chrono::steady_clock::now());
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
