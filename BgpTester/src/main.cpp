#include "ui/MainWindow.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QFont>
#include <QMessageBox>
#include <QStyleFactory>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("BgpTester"));
    QCoreApplication::setApplicationName(QStringLiteral("BgpTester"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    auto font = application.font();
    font.setPointSize(10);
    application.setFont(font);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("统一的 BGP 拓扑编辑与协议仿真工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("topology"), QStringLiteral("启动时打开的拓扑 JSON"), QStringLiteral("[topology.json]"));
    parser.process(application);

    const auto pluginErrors = bgptester::RouterPluginRegistry::instance().registrationErrors();

    bgptester::MainWindow window;
    if (!parser.positionalArguments().isEmpty())
    {
        window.openTopologyPath(QFileInfo(parser.positionalArguments().front()).absoluteFilePath());
    }
    window.show();
    if (!pluginErrors.isEmpty())
    {
        QMessageBox::warning(&window, QStringLiteral("部分路由器插件注册失败"), pluginErrors.join(u'\n'));
    }
    return application.exec();
}
