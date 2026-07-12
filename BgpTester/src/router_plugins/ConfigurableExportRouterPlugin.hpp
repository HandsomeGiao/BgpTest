#pragma once

#include "plugin/RouterPlugin.hpp"

namespace bgptester {

// A small reference plugin showing the complete two-file extension pattern.
class ConfigurableExportRouterPlugin final : public RouterNodePlugin {
public:
  [[nodiscard]] RouterPluginMetadata metadata() const override;
  [[nodiscard]] RouterNode *
  createRouterNode(const RouterNodeContext &context, QObject *parent,
                   QString *error) override;
};

} // namespace bgptester
