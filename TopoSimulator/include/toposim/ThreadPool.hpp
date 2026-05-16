#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace toposim {

class ThreadPool {
public:
  explicit ThreadPool(std::size_t worker_count);
  ~ThreadPool();

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  void enqueue(std::function<void()> task);
  bool waitForIdle(std::chrono::milliseconds timeout,
                   std::chrono::milliseconds quiet_period);
  [[nodiscard]] bool isIdleFor(std::chrono::milliseconds quiet_period) const;
  void stop();
  [[nodiscard]] std::size_t pending() const;

private:
  void workerLoop();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  std::atomic<std::size_t> pending_{0};
  std::chrono::steady_clock::time_point idle_since_;
  bool stopping_ = false;
};

} // namespace toposim
