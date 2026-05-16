#include "toposim/BgpTypes.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace toposim {

std::string toString(BgpMessageType type) {
  switch (type) {
  case BgpMessageType::Open:
    return "OPEN";
  case BgpMessageType::Update:
    return "UPDATE";
  case BgpMessageType::Notification:
    return "NOTIFICATION";
  }
  return "UNKNOWN";
}

std::string toString(SessionType type) {
  return type == SessionType::Ebgp ? "ebgp" : "ibgp";
}

std::string toString(PeerState state) {
  switch (state) {
  case PeerState::Idle:
    return "Idle";
  case PeerState::OpenSent:
    return "OpenSent";
  case PeerState::Established:
    return "Established";
  }
  return "Unknown";
}

BgpMessageType bgpMessageTypeFromString(const std::string &value) {
  std::string upper = value;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](char ch) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  });
  if (upper == "OPEN") {
    return BgpMessageType::Open;
  }
  if (upper == "UPDATE") {
    return BgpMessageType::Update;
  }
  if (upper == "NOTIFICATION") {
    return BgpMessageType::Notification;
  }
  throw std::invalid_argument("Unknown BGP message type: " + value);
}

SessionType sessionTypeFromString(const std::string &value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  if (lower == "ebgp") {
    return SessionType::Ebgp;
  }
  if (lower == "ibgp") {
    return SessionType::Ibgp;
  }
  throw std::invalid_argument("Unknown session type: " + value);
}

} // namespace toposim
