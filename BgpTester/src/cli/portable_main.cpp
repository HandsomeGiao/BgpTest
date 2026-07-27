#include "core/HeadlessSession.hpp"
#include "core/RouterPolicy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
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
#ifdef _WIN32
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(::fileno(stdin)) != 0;
#endif
}

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string currentTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    auto fraction = static_cast<int>((milliseconds - seconds).count());
    if (fraction < 0)
    {
        fraction += 1000;
        seconds -= std::chrono::seconds(1);
    }
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(seconds));
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S.") << std::setfill('0') << std::setw(3) << fraction << 'Z';
    return result.str();
}

std::string fileTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    auto fraction = static_cast<int>((milliseconds - seconds).count());
    if (fraction < 0)
    {
        fraction += 1000;
        seconds -= std::chrono::seconds(1);
    }
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(seconds));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream result;
    result << std::put_time(&local, "%Y%m%d_%H%M%S_") << std::setfill('0') << std::setw(3) << fraction;
    return result.str();
}

class JsonlOutput final
{
public:
    bool open(const std::string& recordPath, std::string* error)
    {
        if (recordPath.empty())
        {
            return true;
        }
        std::error_code filesystemError;
        const auto absolute = std::filesystem::absolute(recordPath, filesystemError);
        if (filesystemError)
        {
            *error = "无法解析会话记录路径：" + filesystemError.message();
            return false;
        }
        if (!absolute.parent_path().empty())
        {
            std::filesystem::create_directories(absolute.parent_path(), filesystemError);
            if (filesystemError)
            {
                *error = "无法创建会话记录目录：" + absolute.parent_path().string() + "：" + filesystemError.message();
                return false;
            }
        }
        const auto alreadyExists = std::filesystem::exists(absolute, filesystemError);
        if (filesystemError)
        {
            *error = "无法检查会话记录路径：" + absolute.string() + "：" + filesystemError.message();
            return false;
        }
        if (alreadyExists)
        {
            *error = "会话记录已存在：" + absolute.string();
            return false;
        }
        record_.open(absolute, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!record_)
        {
            *error = "无法创建会话记录：" + absolute.string();
            return false;
        }
        recordPath_ = absolute.string();
        return true;
    }

    bool write(const Json& object)
    {
        const auto line = object.dump() + '\n';
        if (record_.is_open())
        {
            record_.write(line.data(), static_cast<std::streamsize>(line.size()));
            record_.flush();
            if (!record_)
            {
                lastError_ = "写入会话审计文件失败";
                return false;
            }
        }
        std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
        std::cout.flush();
        if (!std::cout)
        {
            lastError_ = "写入标准输出失败";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& recordPath() const noexcept { return recordPath_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

private:
    std::ofstream record_;
    std::string recordPath_;
    std::string lastError_;
};

struct CommandSource
{
    Json command = Json::object();
    std::string source;
    int line = 0;
};

Json parseCommand(std::string_view text, std::string* error)
{
    error->clear();
    const auto normalized = trim(text);
    if (normalized.empty())
    {
        return Json::object();
    }
    if (normalized.front() != '{')
    {
        return Json{{"command", normalized}};
    }
    try
    {
        auto command = Json::parse(normalized);
        if (!command.is_object())
        {
            *error = "命令必须是 JSON 对象";
            return Json::object();
        }
        return command;
    }
    catch (const Json::parse_error& parseError)
    {
        *error = "JSON 解析失败（偏移 " + std::to_string(parseError.byte) + "）：" + parseError.what();
        return Json::object();
    }
}

bool readScript(const std::string& path, std::vector<CommandSource>* commands, std::string* error,
                std::string* errorRaw = nullptr, int* errorLine = nullptr)
{
    if (errorRaw)
    {
        errorRaw->clear();
    }
    if (errorLine)
    {
        *errorLine = 0;
    }
    std::error_code filesystemError;
    const auto absolute = std::filesystem::absolute(path, filesystemError);
    if (filesystemError)
    {
        *error = "无法解析命令脚本路径 " + path + "：" + filesystemError.message();
        return false;
    }
    std::ifstream input(absolute, std::ios::binary);
    if (!input)
    {
        *error = "无法读取命令脚本 " + absolute.string();
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
    {
        *error = "读取命令脚本失败 " + absolute.string();
        return false;
    }
    try
    {
        const auto document = Json::parse(contents);
        if (document.is_array() || document.is_object())
        {
            const auto values = document.is_array() ? document : Json::array({document});
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                if (!values[index].is_object())
                {
                    *error = "命令脚本 JSON 数组第 " + std::to_string(index + 1) + " 项不是对象";
                    if (errorRaw)
                    {
                        *errorRaw = values[index].dump();
                    }
                    if (errorLine)
                    {
                        *errorLine = static_cast<int>(index + 1);
                    }
                    return false;
                }
                commands->push_back(CommandSource{values[index], absolute.string(), static_cast<int>(index + 1)});
            }
            return true;
        }
    }
    catch (const Json::exception&)
    {
        // A JSONL document is expected to fail whole-document parsing.
    }

    std::istringstream lines(contents);
    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(lines, rawLine))
    {
        ++lineNumber;
        const auto normalized = trim(rawLine);
        if (normalized.empty() || normalized.front() == '#')
        {
            continue;
        }
        std::string lineError;
        auto command = parseCommand(normalized, &lineError);
        if (!lineError.empty())
        {
            *error = absolute.string() + ':' + std::to_string(lineNumber) + "：" + lineError;
            if (errorRaw)
            {
                *errorRaw = rawLine;
            }
            if (errorLine)
            {
                *errorLine = lineNumber;
            }
            return false;
        }
        commands->push_back(CommandSource{std::move(command), absolute.string(), lineNumber});
    }
    return true;
}

Json resultEnvelope(std::uint64_t sequence, const CommandSource& source, const HeadlessCommandResult& result,
                    std::int64_t durationMs)
{
    Json envelope{{"type", "command_result"},
                  {"sequence", sequence},
                  {"timestamp", currentTimestamp()},
                  {"source", source.source},
                  {"line", source.line},
                  {"duration_ms", durationMs},
                  {"command", source.command},
                  {"ok", result.ok},
                  {"data", result.data}};
    if (!result.error.empty())
    {
        envelope["error"] = result.error;
    }
    return envelope;
}

Json commandStartedEnvelope(std::uint64_t sequence, const CommandSource& source)
{
    return Json{{"type", "command_started"},
                {"sequence", sequence},
                {"timestamp", currentTimestamp()},
                {"source", source.source},
                {"line", source.line},
                {"command", source.command}};
}

Json protocolErrorEnvelope(std::uint64_t sequence, const std::string& source, int line, const std::string& raw,
                           const std::string& error)
{
    return Json{{"type", "protocol_error"},
                {"sequence", sequence},
                {"timestamp", currentTimestamp()},
                {"source", source},
                {"line", line},
                {"raw", raw},
                {"ok", false},
                {"error", error}};
}

std::string defaultRecordPath()
{
    return (std::filesystem::current_path() / "tmp" / "cli_sessions" /
            ("bgptester_cli_" + fileTimestamp() + ".jsonl"))
        .string();
}

struct CommandLine
{
    std::string topology;
    std::string script;
    std::vector<std::string> execute;
    std::string record;
    bool recordSet = false;
    bool noRecord = false;
    bool keepGoing = false;
    bool help = false;
    bool version = false;
};

void printUsage(std::ostream& output)
{
    output << "BgpTesterCli - BgpTester 无 UI JSONL 命令行模式\n\n"
              "Usage: BgpTesterCli [options] [topology.json]\n\n"
              "  -t, --topology <path>   会话开始时加载的拓扑 JSON\n"
              "  -s, --script <path>     JSON 数组或 JSONL 命令脚本；- 表示标准输入\n"
              "  -e, --execute <command> 执行一个 JSON 对象或无参数命令；可重复\n"
              "  -r, --record <path>     命令与结果审计 JSONL\n"
              "      --no-record         不创建会话审计文件\n"
              "  -k, --keep-going        脚本命令失败后继续执行\n"
              "  -h, --help              显示帮助\n"
              "  -v, --version           显示版本\n";
}

std::optional<CommandLine> parseCommandLine(int argc, char* argv[], std::string* error)
{
    CommandLine result;
    bool positionalOnly = false;
    std::vector<std::string> positional;
    const auto takeValue = [&](int* index, std::string_view option) -> std::optional<std::string>
    {
        if (*index + 1 >= argc)
        {
            *error = std::string(option) + " requires a value";
            return std::nullopt;
        }
        return std::string(argv[++*index]);
    };
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (!positionalOnly && argument == "--")
        {
            positionalOnly = true;
        }
        else if (!positionalOnly && (argument == "-h" || argument == "--help"))
        {
            result.help = true;
        }
        else if (!positionalOnly && (argument == "-v" || argument == "--version"))
        {
            result.version = true;
        }
        else if (!positionalOnly && (argument == "-k" || argument == "--keep-going"))
        {
            result.keepGoing = true;
        }
        else if (!positionalOnly && argument == "--no-record")
        {
            result.noRecord = true;
        }
        else if (!positionalOnly && (argument == "-t" || argument == "--topology"))
        {
            auto value = takeValue(&index, argument);
            if (!value)
            {
                return std::nullopt;
            }
            result.topology = std::move(*value);
        }
        else if (!positionalOnly && (argument == "-s" || argument == "--script"))
        {
            auto value = takeValue(&index, argument);
            if (!value)
            {
                return std::nullopt;
            }
            result.script = std::move(*value);
        }
        else if (!positionalOnly && (argument == "-e" || argument == "--execute"))
        {
            auto value = takeValue(&index, argument);
            if (!value)
            {
                return std::nullopt;
            }
            result.execute.push_back(std::move(*value));
        }
        else if (!positionalOnly && (argument == "-r" || argument == "--record"))
        {
            auto value = takeValue(&index, argument);
            if (!value)
            {
                return std::nullopt;
            }
            result.record = std::move(*value);
            result.recordSet = true;
        }
        else if (!positionalOnly && argument.starts_with("--topology="))
        {
            result.topology = argument.substr(11);
        }
        else if (!positionalOnly && argument.starts_with("--script="))
        {
            result.script = argument.substr(9);
        }
        else if (!positionalOnly && argument.starts_with("--execute="))
        {
            result.execute.push_back(argument.substr(10));
        }
        else if (!positionalOnly && argument.starts_with("--record="))
        {
            result.record = argument.substr(9);
            result.recordSet = true;
        }
        else if (!positionalOnly && !argument.empty() && argument.front() == '-')
        {
            *error = "Unknown option: " + argument;
            return std::nullopt;
        }
        else
        {
            positional.push_back(argument);
        }
    }
    if (result.recordSet && result.noRecord)
    {
        *error = "--record and --no-record cannot be used together";
        return std::nullopt;
    }
    if (positional.size() > 1 || (!result.topology.empty() && !positional.empty()))
    {
        *error = "Specify one topology with either --topology or the positional argument";
        return std::nullopt;
    }
    if (result.topology.empty() && !positional.empty())
    {
        result.topology = positional.front();
    }
    return result;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string error;
    const auto commandLine = parseCommandLine(argc, argv, &error);
    if (!commandLine)
    {
        std::cerr << error << '\n';
        return 3;
    }
    if (commandLine->help)
    {
        printUsage(std::cout);
        return 0;
    }
    if (commandLine->version)
    {
        std::cout << "BgpTesterCli 1.0.0\n";
        return 0;
    }

    const auto recordPath = commandLine->noRecord
                                ? std::string{}
                                : (commandLine->recordSet ? commandLine->record : defaultRecordPath());
    JsonlOutput output;
    if (!output.open(recordPath, &error))
    {
        std::cerr << error << '\n';
        return 3;
    }

    std::signal(SIGINT, handleInterrupt);
    HeadlessSession session;
    session.setInterruptionFlag(&interrupted);
    const auto registrationErrors = RouterPolicyRegistry::instance().registrationErrors();
    if (!output.write(Json{{"type", "session_started"},
                           {"timestamp", currentTimestamp()},
                           {"protocol", "bgptester-cli-jsonl-v1"},
                           {"application_version", "1.0.0"},
                           {"working_directory", std::filesystem::current_path().string()},
                           {"record_path", output.recordPath()},
                           {"plugin_registration_errors", registrationErrors}}))
    {
        std::cerr << output.lastError() << '\n';
        session.shutdown();
        return 2;
    }

    std::uint64_t sequence = 0;
    int exitCode = 0;
    bool exitRequested = false;
    bool outputFailureReported = false;
    const auto reportOutputFailure = [&]
    {
        exitCode = 2;
        exitRequested = true;
        if (!outputFailureReported)
        {
            std::cerr << output.lastError() << '\n';
            outputFailureReported = true;
        }
    };
    const auto emitProtocolError = [&](const std::string& source, int line, const std::string& raw,
                                       const std::string& message)
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
        const auto startedAt = std::chrono::steady_clock::now();
        const auto result = session.execute(source.command);
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
        if (!output.write(resultEnvelope(commandSequence, source, result, duration.count())))
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

    if (!commandLine->topology.empty())
    {
        const CommandSource source{Json{{"command", "load"}, {"path", commandLine->topology}, {"force", true}},
                                   "command-line", 0};
        if (!runOne(source))
        {
            exitRequested = true;
        }
    }

    std::vector<CommandSource> queuedCommands;
    if (!exitRequested)
    {
        for (std::size_t index = 0; index < commandLine->execute.size(); ++index)
        {
            std::string parseError;
            auto command = parseCommand(commandLine->execute[index], &parseError);
            if (!parseError.empty())
            {
                emitProtocolError("--execute", static_cast<int>(index + 1), commandLine->execute[index], parseError);
                if (!commandLine->keepGoing || exitRequested)
                {
                    exitRequested = true;
                    break;
                }
                continue;
            }
            queuedCommands.push_back(CommandSource{std::move(command), "--execute", static_cast<int>(index + 1)});
        }
    }

    if (!exitRequested && !commandLine->script.empty() && commandLine->script != "-")
    {
        std::string errorRaw;
        int errorLine = 0;
        if (!readScript(commandLine->script, &queuedCommands, &error, &errorRaw, &errorLine))
        {
            std::error_code filesystemError;
            const auto absolute = std::filesystem::absolute(commandLine->script, filesystemError);
            const auto source = filesystemError ? commandLine->script : absolute.string();
            emitProtocolError(source, errorLine, errorRaw, error);
            exitRequested = true;
        }
    }

    for (const auto& source : queuedCommands)
    {
        if (exitRequested || interrupted.load(std::memory_order_relaxed))
        {
            break;
        }
        if (!runOne(source) && !commandLine->keepGoing)
        {
            break;
        }
    }

    const auto readStandardInput = !exitRequested &&
                                   (commandLine->script == "-" ||
                                    (commandLine->script.empty() && queuedCommands.empty()));
    if (readStandardInput)
    {
        const auto interactive = standardInputIsTerminal();
        if (interactive)
        {
            std::cerr << "BgpTesterCli JSONL session. Type help or a JSON command; exit to finish.\n";
        }
        std::string line;
        int lineNumber = 0;
        while (!exitRequested && !interrupted.load(std::memory_order_relaxed))
        {
            if (interactive)
            {
                std::cerr << "bgptester> " << std::flush;
            }
            if (!std::getline(std::cin, line))
            {
                break;
            }
            ++lineNumber;
            const auto normalized = trim(line);
            if (normalized.empty() || normalized.front() == '#')
            {
                continue;
            }
            std::string parseError;
            auto command = parseCommand(normalized, &parseError);
            if (!parseError.empty())
            {
                emitProtocolError("stdin", lineNumber, line, parseError);
                if (exitRequested || (!interactive && !commandLine->keepGoing))
                {
                    break;
                }
                continue;
            }
            const auto ok = runOne(CommandSource{std::move(command), "stdin", lineNumber});
            if (!ok && !interactive && !commandLine->keepGoing)
            {
                break;
            }
        }
    }

    if (interrupted.load(std::memory_order_relaxed))
    {
        exitCode = 130;
    }
    std::string shutdownError;
    if (!session.shutdown(&shutdownError) && exitCode != 130)
    {
        exitCode = 2;
    }
    Json finished{{"type", "session_finished"},
                  {"timestamp", currentTimestamp()},
                  {"exit_code", exitCode},
                  {"interrupted", interrupted.load(std::memory_order_relaxed)},
                  {"status", session.statusJson()}};
    if (!shutdownError.empty())
    {
        finished["shutdown_error"] = shutdownError;
    }
    if (!output.write(finished))
    {
        if (exitCode != 130)
        {
            exitCode = 2;
        }
        if (!outputFailureReported)
        {
            std::cerr << output.lastError() << '\n';
        }
    }
    return exitCode;
}
