#include "toposim/BmpCollector.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace toposim {
namespace {

std::string timestampNow() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;

  std::tm tm{};
  gmtime_s(&tm, &time);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
      << std::setfill('0') << millis.count() << "Z";
  return oss.str();
}

} // namespace

BmpCollector::BmpCollector(std::filesystem::path log_file)
    : log_file_(std::move(log_file)) {
  std::filesystem::create_directories(log_file_.parent_path());
  out_.open(log_file_, std::ios::out | std::ios::app);
  if (!out_) {
    throw std::runtime_error("Unable to open BMP collector log: " +
                             log_file_.string());
  }
}

BmpCollector::~BmpCollector() {
  std::lock_guard lock(mutex_);
  out_.flush();
}

void BmpCollector::recordReceive(const std::string &router_id,
                                 const BgpMessage &message) {
  writeLine({
      {"timestamp", timestampNow()},
      {"event", "message_received"},
      {"router", router_id},
      {"from", message.from},
      {"to", message.to},
      {"msg_type", toString(message.type)},
      {"sequence", message.sequence},
      {"message", message},
  });
}

void BmpCollector::recordTopologyEvent(const std::string &event_name,
                                       const nlohmann::json &detail) {
  writeLine({
      {"timestamp", timestampNow()},
      {"event", event_name},
      {"detail", detail},
  });
}

const std::filesystem::path &BmpCollector::logFile() const { return log_file_; }

void BmpCollector::writeLine(nlohmann::json line) {
  std::lock_guard lock(mutex_);
  out_ << line.dump() << '\n';
  out_.flush();
}

} // namespace toposim
