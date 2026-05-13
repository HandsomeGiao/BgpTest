#include <chrono>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "toposim/TopoManager.hpp"

namespace {

std::vector<std::string> split(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> parts;
    std::string part;
    while (iss >> part) {
        parts.push_back(part);
    }
    return parts;
}

void printHelp() {
    std::cout
        << "Commands:\n"
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

std::string topologyPathFromArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--topology" || arg == "-t") && i + 1 < argc) {
            return argv[++i];
        }
        if (arg == "--debug") {
            spdlog::set_level(spdlog::level::debug);
        }
    }
    return "config/sample_topology.json";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto topology_path = topologyPathFromArgs(argc, argv);
        auto topology = toposim::TopoManager::loadTopology(topology_path);
        toposim::TopoManager manager(std::move(topology));

        manager.start();
        const bool converged = manager.waitForConvergence(std::chrono::seconds(20));
        std::cout << "Initial convergence: " << (converged ? "ok" : "timeout") << '\n';
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
                } else if (parts.size() == 2 && parts[0] == "show" && parts[1] == "routers") {
                    std::cout << manager.routersSnapshot().dump(2) << '\n';
                } else if (parts.size() == 3 && parts[0] == "show" && parts[1] == "rib") {
                    std::cout << manager.ribSnapshot(parts[2]).dump(2) << '\n';
                } else if (parts.size() == 3 && parts[0] == "show" && parts[1] == "peers") {
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
                    const bool ok = manager.waitForConvergence(std::chrono::milliseconds(timeout));
                    std::cout << (ok ? "converged" : "timeout") << '\n';
                } else {
                    std::cout << "Unknown command. Type 'help'.\n";
                }
            } catch (const std::exception& ex) {
                std::cerr << "Command failed: " << ex.what() << '\n';
            }
        }

        manager.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << '\n';
        return 1;
    }
}
