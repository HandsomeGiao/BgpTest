#include "toposim/TopologyObserverServer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>

namespace toposim {
namespace {

HANDLE asHandle(void *handle) { return static_cast<HANDLE>(handle); }

std::string win32ErrorMessage(DWORD error) {
  LPSTR buffer = nullptr;
  const DWORD size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message =
      size == 0 || buffer == nullptr ? "unknown error" : std::string(buffer);
  if (buffer) {
    LocalFree(buffer);
  }
  while (!message.empty() &&
         (message.back() == '\r' || message.back() == '\n')) {
    message.pop_back();
  }
  return message;
}

std::string routeKey(const std::string &router, const std::string &prefix) {
  return router + '\n' + prefix;
}

bool writeMessage(HANDLE pipe, const std::string &message) {
  const auto framed = message + '\n';
  DWORD written = 0;
  return WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()),
                   &written, nullptr) &&
         static_cast<std::size_t>(written) == framed.size();
}

} // namespace

TopologyObserverServer::TopologyObserverServer(std::string pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

TopologyObserverServer::~TopologyObserverServer() { stop(); }

void TopologyObserverServer::start(nlohmann::json topology,
                                   std::string topology_path) {
  std::lock_guard lock(mutex_);
  if (running_) {
    return;
  }

  topology_ = std::move(topology);
  topology_path_ = std::move(topology_path);
  pending_messages_.clear();
  latest_route_messages_.clear();
  stopping_ = false;

  const auto path = pipePath();
  HANDLE pipe = CreateNamedPipeW(
      path.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 64 * 1024,
      64 * 1024, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    throw std::runtime_error("Failed to start topology observer pipe '" +
                             pipe_name_ + "': " +
                             win32ErrorMessage(error));
  }

  pipe_handle_ = pipe;
  running_ = true;
  server_thread_ = std::thread([this] { serverLoop(); });
}

void TopologyObserverServer::stop() {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return;
    }
    stopping_ = true;
    pipe = asHandle(pipe_handle_);
  }
  cv_.notify_all();

  if (pipe != INVALID_HANDLE_VALUE) {
    HANDLE client = CreateFileW(pipePath().c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (client != INVALID_HANDLE_VALUE) {
      CloseHandle(client);
    }
  }

  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  {
    std::lock_guard lock(mutex_);
    if (pipe_handle_) {
      CloseHandle(asHandle(pipe_handle_));
      pipe_handle_ = nullptr;
    }
    pending_messages_.clear();
    latest_route_messages_.clear();
    topology_path_.clear();
    client_connected_ = false;
    running_ = false;
    stopping_ = false;
  }
}

void TopologyObserverServer::publishBestPath(
    const TopoManager::BestPathSnapshot &snapshot) {
  const auto message = bestPathMessage(snapshot);
  {
    std::lock_guard lock(mutex_);
    latest_route_messages_[routeKey(snapshot.router, snapshot.prefix)] =
        message;
  }
  enqueueMessage(message);
}

const std::string &TopologyObserverServer::pipeName() const {
  return pipe_name_;
}

void TopologyObserverServer::serverLoop() {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(mutex_);
    pipe = asHandle(pipe_handle_);
  }

  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        break;
      }
    }

    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
    if (!connected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    {
      std::lock_guard lock(mutex_);
      client_connected_ = true;
      pending_messages_.clear();
    }
    sendInitialSnapshot(pipe);

    bool connected_client = true;
    while (connected_client) {
      std::deque<std::string> messages;
      {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
          return stopping_ || !pending_messages_.empty();
        });
        if (stopping_) {
          connected_client = false;
          break;
        }
        messages.swap(pending_messages_);
      }

      if (messages.empty()) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
          connected_client = false;
        }
        continue;
      }

      for (const auto &message : messages) {
        if (!writeMessage(pipe, message)) {
          connected_client = false;
          break;
        }
      }
    }

    DisconnectNamedPipe(pipe);
    {
      std::lock_guard lock(mutex_);
      client_connected_ = false;
      pending_messages_.clear();
    }
  }
}

void TopologyObserverServer::enqueueMessage(std::string message) {
  {
    std::lock_guard lock(mutex_);
    if (!running_ || stopping_ || !client_connected_) {
      return;
    }
    pending_messages_.push_back(std::move(message));
  }
  cv_.notify_all();
}

void TopologyObserverServer::sendInitialSnapshot(void *pipe_handle) {
  std::vector<std::string> messages;
  {
    std::lock_guard lock(mutex_);
    messages.push_back(topologyMessage());
    for (const auto &[_, message] : latest_route_messages_) {
      messages.push_back(message);
    }
  }

  const auto pipe = asHandle(pipe_handle);
  for (const auto &message : messages) {
    if (!writeMessage(pipe, message)) {
      return;
    }
  }
}

std::string TopologyObserverServer::topologyMessage() const {
  nlohmann::json message;
  message["type"] = "topology";
  message["topology"] = topology_;
  if (!topology_path_.empty()) {
    message["topology_path"] = topology_path_;
  }
  return message.dump();
}

std::string TopologyObserverServer::bestPathMessage(
    const TopoManager::BestPathSnapshot &snapshot) const {
  nlohmann::json message;
  message["type"] = "best_path";
  message["router"] = snapshot.router;
  message["prefix"] = snapshot.prefix;
  message["valid"] = snapshot.valid;
  if (snapshot.route) {
    const auto &route = *snapshot.route;
    message["learned_from"] = route.learned_from;
    message["local_origin"] = route.local_origin;
    message["source_session"] = toString(route.source_session);
    message["next_hop"] = route.attributes.next_hop;
    message["as_path"] = route.attributes.as_path;
    message["local_pref"] = route.attributes.local_pref;
    message["med"] = route.attributes.med;
  }
  return message.dump();
}

std::wstring TopologyObserverServer::pipePath() const {
  std::wstring wide_name(pipe_name_.begin(), pipe_name_.end());
  return L"\\\\.\\pipe\\" + wide_name;
}

} // namespace toposim
