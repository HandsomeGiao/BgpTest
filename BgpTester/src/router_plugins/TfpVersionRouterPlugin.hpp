#pragma once

#include "plugin/RouterPlugin.hpp"

namespace bgptester
{

inline const QString TfpVersionRouterPluginId = QStringLiteral("org.bgptester.router.tfp-version");

class TfpVersionRouterPlugin final : public RouterNodePlugin
{
public:
    RouterPluginMetadata metadata() const override;
    RouterNode* createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) override;
};

} // namespace bgptester
