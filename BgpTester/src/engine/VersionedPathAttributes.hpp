#pragma once

#include <nlohmann/json.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bgptester
{

struct StateCreator
{
    std::uint32_t asn = 0;
    std::string routerId;
    auto operator<=>(const StateCreator&) const = default;
};

struct VersionedState
{
    StateCreator creator;
    std::uint64_t version = 0;
    auto operator<=>(const VersionedState&) const = default;
};

// path aligns with compressed AS_PATH. advertiser additionally identifies an
// iBGP declaration (including an originated route with an empty AS_PATH).
struct VersionedPath
{
    VersionedState advertiser;
    std::vector<VersionedState> path;
    bool operator==(const VersionedPath&) const = default;
};

enum class StateRecordKind { RetireBefore, Revoke };

struct StateRecord
{
    std::uint16_t afi = 1;
    std::uint8_t safi = 1;
    std::string prefix;
    StateRecordKind kind = StateRecordKind::RetireBefore;
    VersionedState state;
    auto operator<=>(const StateRecord&) const = default;
};

struct WithdrawalBinding
{
    VersionedState advertiser;
    std::optional<VersionedState> pathHead;
    bool operator==(const WithdrawalBinding&) const = default;
};

std::vector<VersionedState> declarationPath(const VersionedPath& info);
nlohmann::json stateJson(const VersionedState& state);
nlohmann::json versionedPathJson(const VersionedPath& info);
nlohmann::json stateRecordJson(const StateRecord& record);
nlohmann::json withdrawalBindingJson(const WithdrawalBinding& binding);

} // namespace bgptester
