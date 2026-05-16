#include "toposim/BmpCollector.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

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

std::string jsonString(std::string_view value) {
  std::ostringstream oss;
  oss << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      oss << "\\\"";
      break;
    case '\\':
      oss << "\\\\";
      break;
    case '\b':
      oss << "\\b";
      break;
    case '\f':
      oss << "\\f";
      break;
    case '\n':
      oss << "\\n";
      break;
    case '\r':
      oss << "\\r";
      break;
    case '\t':
      oss << "\\t";
      break;
    default:
      if (ch < 0x20) {
        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(ch) << std::dec;
      } else {
        oss << static_cast<char>(ch);
      }
      break;
    }
  }
  oss << '"';
  return oss.str();
}

std::string stringArrayJson(const std::vector<std::string> &values) {
  std::ostringstream oss;
  oss << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      oss << ',';
    }
    oss << jsonString(values[i]);
  }
  oss << ']';
  return oss.str();
}

std::string uintArrayJson(const std::vector<std::uint32_t> &values) {
  std::ostringstream oss;
  oss << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      oss << ',';
    }
    oss << values[i];
  }
  oss << ']';
  return oss.str();
}

std::string stringMapJson(const std::map<std::string, std::string> &values) {
  std::ostringstream oss;
  oss << '{';
  bool first = true;
  for (const auto &[key, value] : values) {
    if (!first) {
      oss << ',';
    }
    first = false;
    oss << jsonString(key) << ':' << jsonString(value);
  }
  oss << '}';
  return oss.str();
}

std::string pathAttributesJson(const PathAttributes &attrs) {
  std::ostringstream oss;
  oss << '{' << jsonString("origin") << ':' << jsonString(attrs.origin) << ','
      << jsonString("as_path") << ':' << uintArrayJson(attrs.as_path) << ','
      << jsonString("next_hop") << ':' << jsonString(attrs.next_hop) << ','
      << jsonString("local_pref") << ':' << attrs.local_pref << ','
      << jsonString("med") << ':' << attrs.med << ','
      << jsonString("communities") << ':' << stringMapJson(attrs.communities);
  if (attrs.originator_id) {
    oss << ',' << jsonString("originator_id") << ':'
        << jsonString(*attrs.originator_id);
  }
  if (attrs.cluster_list) {
    oss << ',' << jsonString("cluster_list") << ':'
        << jsonString(*attrs.cluster_list);
  }
  oss << '}';
  return oss.str();
}

std::string messageJson(const BgpMessage &message) {
  std::ostringstream oss;
  oss << '{' << jsonString("type") << ':' << jsonString(toString(message.type))
      << ',' << jsonString("from") << ':' << jsonString(message.from) << ','
      << jsonString("to") << ':' << jsonString(message.to) << ','
      << jsonString("sequence") << ':' << message.sequence;
  if (message.open) {
    const auto &open = *message.open;
    oss << ',' << jsonString("open") << ":{"
        << jsonString("version") << ':' << open.version << ','
        << jsonString("asn") << ':' << open.asn << ','
        << jsonString("hold_time_seconds") << ':'
        << open.hold_time_seconds << ',' << jsonString("router_id") << ':'
        << jsonString(open.router_id) << '}';
  }
  if (message.update) {
    const auto &update = *message.update;
    oss << ',' << jsonString("update") << ":{"
        << jsonString("withdrawn_routes") << ':'
        << stringArrayJson(update.withdrawn_routes) << ','
        << jsonString("nlri") << ':' << stringArrayJson(update.nlri) << ','
        << jsonString("path_attributes") << ':'
        << pathAttributesJson(update.path_attributes) << '}';
  }
  if (message.notification) {
    const auto &notification = *message.notification;
    oss << ',' << jsonString("notification") << ":{"
        << jsonString("error_code") << ':' << notification.error_code << ','
        << jsonString("error_subcode") << ':' << notification.error_subcode
        << ',' << jsonString("data") << ':'
        << jsonString(notification.data) << '}';
  }
  oss << '}';
  return oss.str();
}

std::string eventValueJson(const BmpEventValue &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return jsonString(item);
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else {
          return std::to_string(item);
        }
      },
      value);
}

std::string eventDetailJson(const BmpEventDetail &detail) {
  std::ostringstream oss;
  oss << '{';
  bool first = true;
  for (const auto &[key, value] : detail) {
    if (!first) {
      oss << ',';
    }
    first = false;
    oss << jsonString(key) << ':' << eventValueJson(value);
  }
  oss << '}';
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
  std::ostringstream line;
  line << '{' << jsonString("timestamp") << ':' << jsonString(timestampNow())
       << ',' << jsonString("event") << ':' << jsonString("message_received")
       << ',' << jsonString("router") << ':' << jsonString(router_id) << ','
       << jsonString("from") << ':' << jsonString(message.from) << ','
       << jsonString("to") << ':' << jsonString(message.to) << ','
       << jsonString("msg_type") << ':' << jsonString(toString(message.type))
       << ',' << jsonString("sequence") << ':' << message.sequence << ','
       << jsonString("message") << ':' << messageJson(message) << '}';
  writeLine(line.str());
}

void BmpCollector::recordTopologyEvent(const std::string &event_name,
                                       const BmpEventDetail &detail) {
  std::ostringstream line;
  line << '{' << jsonString("timestamp") << ':' << jsonString(timestampNow())
       << ',' << jsonString("event") << ':' << jsonString(event_name) << ','
       << jsonString("detail") << ':' << eventDetailJson(detail) << '}';
  writeLine(line.str());
}

const std::filesystem::path &BmpCollector::logFile() const { return log_file_; }

void BmpCollector::writeLine(const std::string &line) {
  std::lock_guard lock(mutex_);
  out_ << line << '\n';
  out_.flush();
}

} // namespace toposim
