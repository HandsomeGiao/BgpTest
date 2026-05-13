#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "toposim/BgpTypes.hpp"

namespace toposim {

class BmpCollector {
public:
    explicit BmpCollector(std::filesystem::path log_file);
    ~BmpCollector();

    BmpCollector(const BmpCollector&) = delete;
    BmpCollector& operator=(const BmpCollector&) = delete;

    void recordReceive(const std::string& router_id, const BgpMessage& message);
    void recordTopologyEvent(const std::string& event_name, const nlohmann::json& detail);

    [[nodiscard]] const std::filesystem::path& logFile() const;

private:
    void writeLine(nlohmann::json line);

    std::filesystem::path log_file_;
    std::ofstream out_;
    std::mutex mutex_;
};

}  // namespace toposim

