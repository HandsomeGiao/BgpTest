#pragma once

#include "plugin/RouterPlugin.hpp"

#include <QMap>
#include <QReadWriteLock>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>

class QPluginLoader;

namespace bgptester {

class StandardBgpRouterPlugin;

struct RegisteredRouterPlugin {
  RouterPluginMetadata metadata;
  QString source;
};

class RouterPluginRegistry final {
public:
  static RouterPluginRegistry &instance();

  RouterPluginRegistry(const RouterPluginRegistry &) = delete;
  RouterPluginRegistry &operator=(const RouterPluginRegistry &) = delete;
  ~RouterPluginRegistry();

  [[nodiscard]] QVector<RegisteredRouterPlugin> plugins() const;
  [[nodiscard]] std::optional<RouterPluginMetadata>
  metadata(const QString &pluginId) const;
  [[nodiscard]] bool contains(const QString &pluginId) const;

  bool loadPlugin(const QString &filePath, QString *error = nullptr);
  [[nodiscard]] QStringList loadDirectory(const QString &directoryPath);

  [[nodiscard]] RouterNode *createRouterNode(const RouterConfig &config,
                                             const Topology &topology,
                                             QObject *parent,
                                             QString *error = nullptr) const;

private:
  RouterPluginRegistry();

  struct Entry {
    RouterPluginMetadata metadata;
    RouterNodePlugin *factory = nullptr;
    QString source;
  };

  bool registerPlugin(RouterNodePlugin *plugin, const QString &source,
                      QString *error);

  mutable QReadWriteLock lock_;
  QMap<QString, Entry> entries_;
  QSet<QString> loadedFiles_;
  QVector<QPluginLoader *> loaders_;
  std::unique_ptr<StandardBgpRouterPlugin> standardPlugin_;
};

} // namespace bgptester
