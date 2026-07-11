#include "ui/MainWindow.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QMessageBox>
#include <QSet>
#include <QStyleFactory>

#include <utility>

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("BgpTester"));
  QCoreApplication::setApplicationName(QStringLiteral("BgpTester"));
  QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
  application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  auto font = application.font();
  font.setPointSize(10);
  application.setFont(font);

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("统一的 BGP 拓扑编辑与协议仿真工具"));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption pluginDirectoryOption(
      QStringList{QStringLiteral("P"), QStringLiteral("router-plugin-dir")},
      QStringLiteral("从指定目录加载路由器插件（可重复）"),
      QStringLiteral("directory"));
  parser.addOption(pluginDirectoryOption);
  parser.addPositionalArgument(QStringLiteral("topology"),
                               QStringLiteral("启动时打开的拓扑 JSON"),
                               QStringLiteral("[topology.json]"));
  parser.process(application);

  QStringList pluginDirectories{
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("plugins/routers")),
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("router-plugins")),
  };
  pluginDirectories.append(parser.values(pluginDirectoryOption));
  pluginDirectories.append(
      qEnvironmentVariable("BGPTESTER_ROUTER_PLUGIN_PATH")
          .split(QDir::listSeparator(), Qt::SkipEmptyParts));
  QSet<QString> visitedDirectories;
  QStringList pluginErrors;
  for (const auto &path : std::as_const(pluginDirectories)) {
    const auto absolutePath = QFileInfo(path).absoluteFilePath();
    if (visitedDirectories.contains(absolutePath)) {
      continue;
    }
    visitedDirectories.insert(absolutePath);
    pluginErrors.append(
        bgptester::RouterPluginRegistry::instance().loadDirectory(
            absolutePath));
  }

  bgptester::MainWindow window;
  if (!parser.positionalArguments().isEmpty()) {
    window.openTopologyPath(
        QFileInfo(parser.positionalArguments().front()).absoluteFilePath());
  }
  window.show();
  if (!pluginErrors.isEmpty()) {
    QMessageBox::warning(
        &window, QStringLiteral("部分路由器插件加载失败"),
        pluginErrors.join(u'\n'));
  }
  return application.exec();
}
