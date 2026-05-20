#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "toposim/TopoManager.hpp"

namespace toposim {

class TopologyObserverServer {
public:
  static constexpr const char *kDefaultPipeName = "TopoSimulatorObserver";

  explicit TopologyObserverServer(
      std::string pipe_name = kDefaultPipeName);
  ~TopologyObserverServer();

  TopologyObserverServer(const TopologyObserverServer &) = delete;
  TopologyObserverServer &operator=(const TopologyObserverServer &) = delete;

  void start(nlohmann::json topology);
  void stop();
  void publishBestPath(const TopoManager::BestPathSnapshot &snapshot);

  [[nodiscard]] const std::string &pipeName() const;

private:
  void serverLoop();
  void enqueueMessage(std::string message);
  void sendInitialSnapshot(void *pipe_handle);
  [[nodiscard]] std::string topologyMessage() const;
  [[nodiscard]] std::string bestPathMessage(
      const TopoManager::BestPathSnapshot &snapshot) const;
  [[nodiscard]] std::wstring pipePath() const;

  std::string pipe_name_;
  nlohmann::json topology_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> pending_messages_;
  std::map<std::string, std::string> latest_route_messages_;
  std::thread server_thread_;
  void *pipe_handle_ = nullptr;
  bool running_ = false;
  bool stopping_ = false;
  bool client_connected_ = false;
};

} // namespace toposim
