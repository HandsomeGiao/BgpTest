#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <variant>

#include "toposim/BgpTypes.hpp"

namespace toposim {

using BmpEventValue = std::variant<std::string, std::uint64_t, bool>;
using BmpEventDetail = std::map<std::string, BmpEventValue>;

class BmpCollector {
public:
    explicit BmpCollector(std::filesystem::path log_file);
    ~BmpCollector();

    BmpCollector(const BmpCollector&) = delete;
    BmpCollector& operator=(const BmpCollector&) = delete;

    void recordReceive(const std::string& router_id, const BgpMessage& message);
    void recordTopologyEvent(const std::string& event_name, const BmpEventDetail& detail);

    [[nodiscard]] const std::filesystem::path& logFile() const;

private:
    void writeLine(const std::string& line);

    std::filesystem::path log_file_;
    std::ofstream out_;
    std::mutex mutex_;
};

}  // namespace toposim
