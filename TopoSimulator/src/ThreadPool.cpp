#include "toposim/ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

namespace toposim {

ThreadPool::ThreadPool(std::size_t worker_count) {
  worker_count = std::max<std::size_t>(1, worker_count);
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
  }
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::enqueue(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      throw std::runtime_error("Cannot enqueue task after ThreadPool stopped");
    }
    pending_.fetch_add(1, std::memory_order_relaxed);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

bool ThreadPool::waitForIdle(std::chrono::milliseconds timeout,
                             std::chrono::milliseconds quiet_period) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto quiet_start = std::optional<std::chrono::steady_clock::time_point>{};

  std::unique_lock lock(mutex_);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pending_.load(std::memory_order_relaxed) == 0 && tasks_.empty()) {
      if (!quiet_start) {
        quiet_start = std::chrono::steady_clock::now();
      }
      const auto quiet_until = *quiet_start + quiet_period;
      if (quiet_until <= std::chrono::steady_clock::now()) {
        return true;
      }
      idle_cv_.wait_until(lock, std::min(deadline, quiet_until));
    } else {
      quiet_start.reset();
      idle_cv_.wait_until(lock, deadline);
    }
  }
  return pending_.load(std::memory_order_relaxed) == 0 && tasks_.empty();
}

void ThreadPool::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

std::size_t ThreadPool::pending() const {
  return pending_.load(std::memory_order_relaxed);
}

void ThreadPool::workerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }

    try {
      task();
    } catch (...) {
      // The simulation should keep running even if a custom hook throws.
    }

    pending_.fetch_sub(1, std::memory_order_relaxed);
    idle_cv_.notify_all();
  }
}

} // namespace toposim
