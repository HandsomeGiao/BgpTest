#pragma once

#include "engine/BgpTypes.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace bgptester
{

class SimulationEventCodec final
{
public:
    SimulationEventCodec() = delete;

    static QJsonObject toJson(const SimulationEvent& event);
    static std::optional<SimulationEvent> fromJson(const QByteArray& json, QString* error = nullptr);
};

} // namespace bgptester
