#include "headless/HeadlessSession.hpp"
#include "plugin/RouterPluginRegistry.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTextStream>

#include <atomic>
#include <csignal>
#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{

using namespace bgptester;

std::atomic_bool interrupted{false};

void handleInterrupt(int)
{
    interrupted.store(true, std::memory_order_relaxed);
}

bool standardInputIsTerminal()
{
#ifdef Q_OS_WIN
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(::fileno(stdin)) != 0;
#endif
}

class JsonlOutput final
{
public:
    bool open(const QString& recordPath, QString* error)
    {
        if (!stdout_.open(stdout, QIODevice::WriteOnly))
        {
            *error = QStringLiteral("无法打开标准输出：%1").arg(stdout_.errorString());
            return false;
        }
        if (recordPath.isEmpty())
        {
            return true;
        }
        const QFileInfo info(recordPath);
        auto directory = info.absoluteDir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
        {
            *error = QStringLiteral("无法创建会话记录目录：%1").arg(directory.absolutePath());
            return false;
        }
        record_.setFileName(info.absoluteFilePath());
        if (!record_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::NewOnly))
        {
            *error = QStringLiteral("无法创建会话记录：%1").arg(record_.errorString());
            return false;
        }
        recordPath_ = info.absoluteFilePath();
        return true;
    }

    bool write(const QJsonObject& object)
    {
        auto line = QJsonDocument(object).toJson(QJsonDocument::Compact);
        line.append('\n');
        bool success = true;
        if (record_.isOpen())
        {
            success = writeDevice(&record_, line, QStringLiteral("会话审计文件")) && success;
        }
        success = writeDevice(&stdout_, line, QStringLiteral("标准输出")) && success;
        return success;
    }

    QString recordPath() const
    {
        return recordPath_;
    }

    QString lastError() const
    {
        return lastError_;
    }

private:
    bool writeDevice(QFile* device, const QByteArray& line, const QString& description)
    {
        const auto written = device->write(line);
        if (written != line.size())
        {
            if (lastError_.isEmpty())
            {
                lastError_ = QStringLiteral("写入%1失败（%2/%3 字节）：%4")
                                 .arg(description)
                                 .arg(written)
                                 .arg(line.size())
                                 .arg(device->errorString());
            }
            return false;
        }
        if (!device->flush())
        {
            if (lastError_.isEmpty())
            {
                lastError_ = QStringLiteral("刷新%1失败：%2").arg(description, device->errorString());
            }
            return false;
        }
        return true;
    }

    QFile stdout_;
    QFile record_;
    QString recordPath_;
    QString lastError_;
};

struct CommandSource
{
    QJsonObject command;
    QString source;
    int line = 0;
};

QJsonObject parseCommand(const QString& text, QString* error)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return {};
    }
    if (!trimmed.startsWith(u'{'))
    {
        return QJsonObject{{QStringLiteral("command"), trimmed}};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        *error = parseError.error == QJsonParseError::NoError
                     ? QStringLiteral("命令必须是 JSON 对象")
                     : QStringLiteral("JSON 解析失败（偏移 %1）：%2").arg(parseError.offset).arg(parseError.errorString());
        return {};
    }
    return document.object();
}

bool readScript(const QString& path, QVector<CommandSource>* commands, QString* error, QString* errorRaw = nullptr,
                int* errorLine = nullptr)
{
    if (errorRaw)
    {
        errorRaw->clear();
    }
    if (errorLine)
    {
        *errorLine = 0;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        *error = QStringLiteral("无法读取命令脚本 %1：%2").arg(QFileInfo(path).absoluteFilePath(), file.errorString());
        return false;
    }
    const auto contents = file.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error == QJsonParseError::NoError && (document.isArray() || document.isObject()))
    {
        const auto values = document.isArray() ? document.array() : QJsonArray{document.object()};
        for (qsizetype index = 0; index < values.size(); ++index)
        {
            if (!values.at(index).isObject())
            {
                *error = QStringLiteral("命令脚本 JSON 数组第 %1 项不是对象").arg(index + 1);
                if (errorRaw)
                {
                    *errorRaw = QString::fromUtf8(QJsonDocument(QJsonArray{values.at(index)}).toJson(QJsonDocument::Compact));
                }
                if (errorLine)
                {
                    *errorLine = static_cast<int>(index + 1);
                }
                return false;
            }
            commands->append(CommandSource{.command = values.at(index).toObject(),
                                           .source = QFileInfo(path).absoluteFilePath(),
                                           .line = static_cast<int>(index + 1)});
        }
        return true;
    }

    int lineNumber = 0;
    const auto lines = QString::fromUtf8(contents).split(u'\n');
    for (const auto& rawLine : lines)
    {
        auto line = rawLine;
        if (line.endsWith(u'\r'))
        {
            line.chop(1);
        }
        ++lineNumber;
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(u'#'))
        {
            continue;
        }
        QString lineError;
        auto command = parseCommand(line, &lineError);
        if (!lineError.isEmpty())
        {
            *error = QStringLiteral("%1:%2：%3").arg(QFileInfo(path).absoluteFilePath()).arg(lineNumber).arg(lineError);
            if (errorRaw)
            {
                *errorRaw = line;
            }
            if (errorLine)
            {
                *errorLine = lineNumber;
            }
            return false;
        }
        commands->append(CommandSource{.command = std::move(command),
                                       .source = QFileInfo(path).absoluteFilePath(),
                                       .line = lineNumber});
    }
    return true;
}

QJsonObject resultEnvelope(quint64 sequence, const CommandSource& source, const HeadlessCommandResult& result, qint64 durationMs)
{
    QJsonObject envelope{{QStringLiteral("type"), QStringLiteral("command_result")},
                         {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                         {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                         {QStringLiteral("source"), source.source},
                         {QStringLiteral("line"), source.line},
                         {QStringLiteral("duration_ms"), durationMs},
                         {QStringLiteral("command"), source.command},
                         {QStringLiteral("ok"), result.ok},
                         {QStringLiteral("data"), result.data}};
    if (!result.error.isEmpty())
    {
        envelope.insert(QStringLiteral("error"), result.error);
    }
    return envelope;
}

QJsonObject commandStartedEnvelope(quint64 sequence, const CommandSource& source)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("command_started")},
                       {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                       {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("source"), source.source},
                       {QStringLiteral("line"), source.line},
                       {QStringLiteral("command"), source.command}};
}

QJsonObject protocolErrorEnvelope(quint64 sequence, const QString& source, int line, const QString& raw, const QString& error)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("protocol_error")},
                       {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                       {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("line"), line},
                       {QStringLiteral("raw"), raw},
                       {QStringLiteral("ok"), false},
                       {QStringLiteral("error"), error}};
}

QString defaultRecordPath()
{
    const auto name = QStringLiteral("bgptester_cli_%1.jsonl").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    return QDir::current().absoluteFilePath(QDir(QStringLiteral("tmp/cli_sessions")).filePath(name));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("BgpTester"));
    QCoreApplication::setApplicationName(QStringLiteral("BgpTesterCli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("BgpTester 无 UI JSONL 命令行模式"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption topologyOption({QStringLiteral("t"), QStringLiteral("topology")},
                                            QStringLiteral("会话开始时加载的拓扑 JSON"), QStringLiteral("path"));
    const QCommandLineOption scriptOption({QStringLiteral("s"), QStringLiteral("script")},
                                          QStringLiteral("JSON 数组或 JSONL 命令脚本；- 表示标准输入"), QStringLiteral("path"));
    const QCommandLineOption executeOption({QStringLiteral("e"), QStringLiteral("execute")},
                                           QStringLiteral("执行一个 JSON 对象或无参数命令；可重复"), QStringLiteral("command"));
    const QCommandLineOption recordOption({QStringLiteral("r"), QStringLiteral("record")},
                                          QStringLiteral("命令与结果审计 JSONL（默认 tmp/cli_sessions）"), QStringLiteral("path"));
    const QCommandLineOption noRecordOption(QStringLiteral("no-record"), QStringLiteral("不创建会话审计文件；stdout 仍为 JSONL"));
    const QCommandLineOption keepGoingOption({QStringLiteral("k"), QStringLiteral("keep-going")},
                                             QStringLiteral("脚本命令失败后继续执行"));
    parser.addOptions({topologyOption, scriptOption, executeOption, recordOption, noRecordOption, keepGoingOption});
    parser.addPositionalArgument(QStringLiteral("topology"), QStringLiteral("兼容 GUI 用法的初始拓扑 JSON"),
                                 QStringLiteral("[topology.json]"));
    parser.process(application);

    if (parser.isSet(recordOption) && parser.isSet(noRecordOption))
    {
        std::fprintf(stderr, "--record and --no-record cannot be used together\n");
        return 3;
    }
    if (parser.positionalArguments().size() > 1 ||
        (parser.isSet(topologyOption) && !parser.positionalArguments().isEmpty()))
    {
        std::fprintf(stderr, "Specify one topology with either --topology or the positional argument\n");
        return 3;
    }

    const auto recordPath = parser.isSet(noRecordOption)
                                ? QString{}
                                : (parser.isSet(recordOption) ? parser.value(recordOption) : defaultRecordPath());
    JsonlOutput output;
    QString error;
    if (!output.open(recordPath, &error))
    {
        std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 3;
    }

    std::signal(SIGINT, handleInterrupt);
    HeadlessSession session;
    session.setInterruptionFlag(&interrupted);
    if (!output.write(QJsonObject{{QStringLiteral("type"), QStringLiteral("session_started")},
                                  {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                                  {QStringLiteral("protocol"), QStringLiteral("bgptester-cli-jsonl-v1")},
                                  {QStringLiteral("application_version"), QCoreApplication::applicationVersion()},
                                  {QStringLiteral("working_directory"), QDir::currentPath()},
                                  {QStringLiteral("record_path"), output.recordPath()},
                                  {QStringLiteral("plugin_registration_errors"),
                                   QJsonArray::fromStringList(RouterPluginRegistry::instance().registrationErrors())}}))
    {
        std::fprintf(stderr, "%s\n", output.lastError().toLocal8Bit().constData());
        session.shutdown();
        return 2;
    }

    quint64 sequence = 0;
    int exitCode = 0;
    bool exitRequested = false;
    bool outputFailureReported = false;
    const auto keepGoing = parser.isSet(keepGoingOption);
    const auto reportOutputFailure = [&]
    {
        exitCode = 2;
        exitRequested = true;
        if (!outputFailureReported)
        {
            std::fprintf(stderr, "%s\n", output.lastError().toLocal8Bit().constData());
            outputFailureReported = true;
        }
    };
    const auto emitProtocolError = [&](const QString& source, int line, const QString& raw, const QString& message)
    {
        if (!output.write(protocolErrorEnvelope(++sequence, source, line, raw, message)))
        {
            reportOutputFailure();
            return false;
        }
        exitCode = 2;
        return true;
    };
    const auto runOne = [&](const CommandSource& source)
    {
        const auto commandSequence = ++sequence;
        if (!output.write(commandStartedEnvelope(commandSequence, source)))
        {
            reportOutputFailure();
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        const auto result = session.execute(source.command);
        if (!output.write(resultEnvelope(commandSequence, source, result, timer.elapsed())))
        {
            reportOutputFailure();
        }
        if (!result.ok)
        {
            exitCode = 2;
        }
        if (result.exitRequested)
        {
            exitRequested = true;
        }
        return result.ok;
    };

    QString topologyPath = parser.value(topologyOption);
    if (topologyPath.isEmpty() && !parser.positionalArguments().isEmpty())
    {
        topologyPath = parser.positionalArguments().front();
    }
    if (!topologyPath.isEmpty())
    {
        const CommandSource source{.command = QJsonObject{{QStringLiteral("command"), QStringLiteral("load")},
                                                          {QStringLiteral("path"), topologyPath},
                                                          {QStringLiteral("force"), true}},
                                   .source = QStringLiteral("command-line"),
                                   .line = 0};
        if (!runOne(source))
        {
            exitRequested = true;
        }
    }

    QVector<CommandSource> queuedCommands;
    if (!exitRequested)
    {
        const auto executeValues = parser.values(executeOption);
        for (qsizetype index = 0; index < executeValues.size(); ++index)
        {
            QString parseError;
            auto command = parseCommand(executeValues.at(index), &parseError);
            if (!parseError.isEmpty())
            {
                emitProtocolError(QStringLiteral("--execute"), static_cast<int>(index + 1), executeValues.at(index), parseError);
                if (!keepGoing || exitRequested)
                {
                    exitRequested = true;
                    break;
                }
                continue;
            }
            queuedCommands.append(CommandSource{.command = std::move(command),
                                                 .source = QStringLiteral("--execute"),
                                                 .line = static_cast<int>(index + 1)});
        }
    }

    const auto scriptPath = parser.value(scriptOption);
    if (!exitRequested && !scriptPath.isEmpty() && scriptPath != QStringLiteral("-"))
    {
        QString errorRaw;
        int errorLine = 0;
        if (!readScript(scriptPath, &queuedCommands, &error, &errorRaw, &errorLine))
        {
            emitProtocolError(QFileInfo(scriptPath).absoluteFilePath(), errorLine, errorRaw, error);
            exitRequested = true;
        }
    }

    for (const auto& source : queuedCommands)
    {
        if (exitRequested || interrupted.load(std::memory_order_relaxed))
        {
            break;
        }
        if (!runOne(source) && !keepGoing)
        {
            break;
        }
    }

    const auto readStandardInput = !exitRequested &&
                                   (scriptPath == QStringLiteral("-") || (scriptPath.isEmpty() && queuedCommands.isEmpty()));
    if (readStandardInput)
    {
        const auto interactive = standardInputIsTerminal();
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly | QIODevice::Text))
        {
            emitProtocolError(QStringLiteral("stdin"), 0, {},
                              QStringLiteral("无法打开标准输入：%1").arg(input.errorString()));
            exitRequested = true;
        }
        else
        {
            if (interactive)
            {
                std::fprintf(stderr, "BgpTesterCli JSONL session. Type help or a JSON command; exit to finish.\n");
            }
            QTextStream stream(&input);
            int lineNumber = 0;
            while (!exitRequested && !interrupted.load(std::memory_order_relaxed))
            {
                if (interactive)
                {
                    std::fprintf(stderr, "bgptester> ");
                    std::fflush(stderr);
                }
                const auto line = stream.readLine();
                if (line.isNull())
                {
                    break;
                }
                ++lineNumber;
                if (line.trimmed().isEmpty() || line.trimmed().startsWith(u'#'))
                {
                    continue;
                }
                QString parseError;
                auto command = parseCommand(line, &parseError);
                if (!parseError.isEmpty())
                {
                    emitProtocolError(QStringLiteral("stdin"), lineNumber, line, parseError);
                    if (exitRequested || (!interactive && !keepGoing))
                    {
                        break;
                    }
                    continue;
                }
                const auto ok = runOne(CommandSource{.command = std::move(command),
                                                      .source = QStringLiteral("stdin"),
                                                      .line = lineNumber});
                if (!ok && !interactive && !keepGoing)
                {
                    break;
                }
            }
        }
    }

    if (interrupted.load(std::memory_order_relaxed))
    {
        exitCode = 130;
    }
    QString shutdownError;
    if (!session.shutdown(&shutdownError) && exitCode != 130)
    {
        exitCode = 2;
    }
    QJsonObject finished{{QStringLiteral("type"), QStringLiteral("session_finished")},
                         {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                         {QStringLiteral("exit_code"), exitCode},
                         {QStringLiteral("interrupted"), interrupted.load(std::memory_order_relaxed)},
                         {QStringLiteral("status"), session.statusJson()}};
    if (!shutdownError.isEmpty())
    {
        finished.insert(QStringLiteral("shutdown_error"), shutdownError);
    }
    if (!output.write(finished))
    {
        if (exitCode != 130)
        {
            exitCode = 2;
        }
        if (!outputFailureReported)
        {
            std::fprintf(stderr, "%s\n", output.lastError().toLocal8Bit().constData());
        }
    }
    return exitCode;
}
