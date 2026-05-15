#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <cstdio>
#include <io.h>

#include <spdlog/spdlog.h>

#include "toposim/TopoManager.hpp"

namespace {

std::vector<std::string> split(const std::string &line) {
  std::istringstream iss(line);
  std::vector<std::string> parts;
  std::string part;
  while (iss >> part) {
    parts.push_back(part);
  }
  return parts;
}

void printHelp() {
  std::cout << "Commands:\n"
            << "  help\n"
            << "  show routers\n"
            << "  show peers <router>\n"
            << "  show rib <router>\n"
            << "  link up <a> <b>\n"
            << "  link down <a> <b>\n"
            << "  node up <router>\n"
            << "  node down <router>\n"
            << "  advertise <router> <prefix>\n"
            << "  withdraw <router> <prefix>\n"
            << "  converge [timeout_ms]\n"
            << "  quit\n";
}

bool stdinIsTerminal() {
  return _isatty(_fileno(stdin)) != 0;
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
    std::cout << "No available topologies in: " << topo_dir.string() << '\n';
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  std::cout << "Available topologies in " << topo_dir.string() << ":\n";
  for (std::size_t i = 0; i < topologies.size(); ++i) {
    std::cout << "  " << (i + 1) << ". " << topologies[i].filename().string()
              << '\n';
  }

  if (topologies.size() == 1) {
    std::cout << "Using topology: " << topologies.front().filename().string()
              << '\n';
    return topologies.front();
  }

  if (!stdinIsTerminal()) {
    std::cout << "Multiple topologies are available. Pass --topology <file> in "
                 "non-interactive mode.\n";
    return std::nullopt;
  }

  std::cout << "Select topology [1-" << topologies.size() << "]: ";
  std::string line;
  if (!std::getline(std::cin, line)) {
    return std::nullopt;
  }

  std::size_t selected = 0;
  try {
    selected = static_cast<std::size_t>(std::stoul(line));
  } catch (...) {
    std::cout << "Invalid selection.\n";
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  if (selected == 0 || selected > topologies.size()) {
    std::cout << "Invalid selection.\n";
    waitBeforeExitIfDoubleClicked();
    return std::nullopt;
  }

  return topologies[selected - 1];
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto topology_path = topologyPathFromArgs(argc, argv);
    if (!topology_path) {
      return 1;
    }
    auto topology = toposim::TopoManager::loadTopology(*topology_path);
    toposim::TopoManager manager(std::move(topology));

    manager.start();
    const bool converged = manager.waitForConvergence(std::chrono::seconds(20));
    std::cout << "Initial convergence: " << (converged ? "ok" : "timeout")
              << '\n';
    std::cout << "BMP collector log: " << manager.logFile().string() << '\n';
    printHelp();

    std::string line;
    while (std::cout << "bgp> " && std::getline(std::cin, line)) {
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
          std::cout << manager.routersSnapshot().dump(2) << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "rib") {
          std::cout << manager.ribSnapshot(parts[2]).dump(2) << '\n';
        } else if (parts.size() == 3 && parts[0] == "show" &&
                   parts[1] == "peers") {
          std::cout << manager.peersSnapshot(parts[2]).dump(2) << '\n';
        } else if (parts.size() == 4 && parts[0] == "link") {
          manager.setLinkState(parts[2], parts[3], parts[1] == "up");
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "node") {
          manager.setRouterState(parts[2], parts[1] == "up");
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "advertise") {
          manager.originatePrefix(parts[1], parts[2]);
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts.size() == 3 && parts[0] == "withdraw") {
          manager.withdrawPrefix(parts[1], parts[2]);
          manager.waitForConvergence(std::chrono::seconds(20));
        } else if (parts[0] == "converge") {
          const auto timeout = parts.size() > 1 ? std::stoi(parts[1]) : 20000;
          const bool ok =
              manager.waitForConvergence(std::chrono::milliseconds(timeout));
          std::cout << (ok ? "converged" : "timeout") << '\n';
        } else {
          std::cout << "Unknown command. Type 'help'.\n";
        }
      } catch (const std::exception &ex) {
        std::cerr << "Command failed: " << ex.what() << '\n';
      }
    }

    manager.stop();
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "Fatal: " << ex.what() << '\n';
    waitBeforeExitIfDoubleClicked();
    return 1;
  }
}
