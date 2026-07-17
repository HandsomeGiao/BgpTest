#include "persistence/SimulationEventCodec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include <limits>
#include <utility>

namespace bgptester
{
namespace
{

constexpr auto TimestampFormat = "yyyy-MM-dd HH:mm:ss.zzz";

const QStringList& requiredKeys()
{
    static const QStringList keys{
        QStringLiteral("id"),        QStringLiteral("timestamp"), QStringLiteral("event"),
        QStringLiteral("router"),    QStringLiteral("from"),      QStringLiteral("to"),
        QStringLiteral("msg_type"),  QStringLiteral("action"),    QStringLiteral("sequence"),
        QStringLiteral("prefixes"),  QStringLiteral("withdrawn"), QStringLiteral("next_hop"),
        QStringLiteral("as_path"),   QStringLiteral("details"),
    };
    return keys;
}

void setError(QString* error, const QString& message)
{
    if (error)
    {
        *error = message;
    }
}

QJsonValue unsigned64ToJson(quint64 value)
{
    if (value <= static_cast<quint64>(std::numeric_limits<qint64>::max()))
    {
        return QJsonValue(static_cast<qint64>(value));
    }
    return QJsonValue(QString::number(value));
}

bool readString(const QJsonObject& object, const QString& key, QString* value, QString* error)
{
    const auto jsonValue = object.value(key);
    if (!jsonValue.isString())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是字符串").arg(key));
        return false;
    }
    *value = jsonValue.toString();
    return true;
}

bool readUnsigned64(const QJsonObject& object, const QString& key, quint64* value, QString* error)
{
    const auto jsonValue = object.value(key);
    if (jsonValue.isDouble())
    {
        const auto integer = jsonValue.toInteger(-1);
        if (integer >= 0)
        {
            *value = static_cast<quint64>(integer);
            return true;
        }
    }
    else if (jsonValue.isString())
    {
        const auto text = jsonValue.toString();
        if (!text.isEmpty())
        {
            bool asciiDecimal = true;
            for (const auto ch : text)
            {
                const auto codeUnit = ch.unicode();
                if (codeUnit < u'0' || codeUnit > u'9')
                {
                    asciiDecimal = false;
                    break;
                }
            }
            if (asciiDecimal)
            {
                bool ok = false;
                const auto decoded = text.toULongLong(&ok, 10);
                if (ok)
                {
                    *value = decoded;
                    return true;
                }
            }
        }
    }

    setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是非负整数或无符号十进制字符串").arg(key));
    return false;
}

bool readUnsigned32Value(const QJsonValue& jsonValue, const QString& key, quint32* value, QString* error)
{
    if (!jsonValue.isDouble())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是 32 位非负整数").arg(key));
        return false;
    }
    const auto integer = jsonValue.toInteger(-1);
    if (integer < 0 || integer > std::numeric_limits<quint32>::max())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是 32 位非负整数").arg(key));
        return false;
    }
    *value = static_cast<quint32>(integer);
    return true;
}

bool readOptionalUnsigned32(const QJsonObject& object, const QString& key, std::optional<quint32>* value, QString* error)
{
    const auto it = object.constFind(key);
    if (it == object.constEnd() || it.value().isNull())
    {
        value->reset();
        return true;
    }

    quint32 decoded = 0;
    if (!readUnsigned32Value(it.value(), key, &decoded, error))
    {
        return false;
    }
    *value = decoded;
    return true;
}

bool readStringList(const QJsonObject& object, const QString& key, QStringList* values, QString* error)
{
    const auto jsonValue = object.value(key);
    if (!jsonValue.isArray())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是字符串数组").arg(key));
        return false;
    }

    QStringList decoded;
    const auto array = jsonValue.toArray();
    decoded.reserve(array.size());
    for (const auto& entry : array)
    {
        if (!entry.isString())
        {
            setError(error, QStringLiteral("SimulationEvent JSON 字段 %1 必须是字符串数组").arg(key));
            return false;
        }
        decoded.append(entry.toString());
    }
    *values = std::move(decoded);
    return true;
}

bool readAsPath(const QJsonObject& object, QVector<quint32>* values, QString* error)
{
    const auto jsonValue = object.value(QStringLiteral("as_path"));
    if (!jsonValue.isArray())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 as_path 必须是 32 位非负整数数组"));
        return false;
    }

    QVector<quint32> decoded;
    const auto array = jsonValue.toArray();
    decoded.reserve(array.size());
    for (const auto& entry : array)
    {
        quint32 asn = 0;
        if (!readUnsigned32Value(entry, QStringLiteral("as_path"), &asn, error))
        {
            return false;
        }
        decoded.append(asn);
    }
    *values = std::move(decoded);
    return true;
}

bool readDetails(const QJsonObject& object, QMap<QString, QString>* values, QString* error)
{
    const auto jsonValue = object.value(QStringLiteral("details"));
    if (!jsonValue.isObject())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 details 必须是对象"));
        return false;
    }

    QMap<QString, QString> decoded;
    const auto details = jsonValue.toObject();
    for (auto it = details.constBegin(); it != details.constEnd(); ++it)
    {
        if (!it.value().isString())
        {
            setError(error, QStringLiteral("SimulationEvent JSON 字段 details.%1 必须是字符串").arg(it.key()));
            return false;
        }
        decoded.insert(it.key(), it.value().toString());
    }
    *values = std::move(decoded);
    return true;
}

} // namespace

QJsonObject SimulationEventCodec::toJson(const SimulationEvent& event)
{
    QJsonArray path;
    for (const auto asn : event.asPath)
    {
        path.append(static_cast<qint64>(asn));
    }
    QJsonArray prefixes;
    for (const auto& prefix : event.prefixes)
    {
        prefixes.append(prefix);
    }
    QJsonArray withdrawn;
    for (const auto& prefix : event.withdrawn)
    {
        withdrawn.append(prefix);
    }
    QJsonObject details;
    for (auto it = event.details.cbegin(); it != event.details.cend(); ++it)
    {
        details.insert(it.key(), it.value());
    }

    QJsonObject object{
        {QStringLiteral("id"), unsigned64ToJson(event.id)},
        {QStringLiteral("timestamp"), event.timestamp.toString(QString::fromLatin1(TimestampFormat))},
        {QStringLiteral("event"), event.event},
        {QStringLiteral("router"), event.router},
        {QStringLiteral("from"), event.from},
        {QStringLiteral("to"), event.to},
        {QStringLiteral("msg_type"), event.messageType},
        {QStringLiteral("action"), event.action},
        {QStringLiteral("sequence"), unsigned64ToJson(event.sequence)},
        {QStringLiteral("prefixes"), prefixes},
        {QStringLiteral("withdrawn"), withdrawn},
        {QStringLiteral("next_hop"), event.nextHop},
        {QStringLiteral("as_path"), path},
        {QStringLiteral("details"), details},
    };
    if (event.fromAs)
    {
        object.insert(QStringLiteral("from_as"), static_cast<qint64>(*event.fromAs));
    }
    if (event.toAs)
    {
        object.insert(QStringLiteral("to_as"), static_cast<qint64>(*event.toAs));
    }
    if (event.localPref)
    {
        object.insert(QStringLiteral("local_pref"), static_cast<qint64>(*event.localPref));
    }
    if (event.med)
    {
        object.insert(QStringLiteral("med"), static_cast<qint64>(*event.med));
    }
    return object;
}

std::optional<SimulationEvent> SimulationEventCodec::fromJson(const QByteArray& json, QString* error)
{
    if (error)
    {
        error->clear();
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        setError(error, QStringLiteral("SimulationEvent JSON 解析失败：%1").arg(parseError.errorString()));
        return std::nullopt;
    }
    if (!document.isObject())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 根节点必须是对象"));
        return std::nullopt;
    }

    const auto object = document.object();
    for (const auto& key : requiredKeys())
    {
        if (!object.contains(key))
        {
            setError(error, QStringLiteral("SimulationEvent JSON 缺少必需字段 %1").arg(key));
            return std::nullopt;
        }
    }

    SimulationEvent event;
    QString timestamp;
    if (!readUnsigned64(object, QStringLiteral("id"), &event.id, error) ||
        !readString(object, QStringLiteral("timestamp"), &timestamp, error) ||
        !readString(object, QStringLiteral("event"), &event.event, error) ||
        !readString(object, QStringLiteral("router"), &event.router, error) ||
        !readString(object, QStringLiteral("from"), &event.from, error) ||
        !readString(object, QStringLiteral("to"), &event.to, error) ||
        !readOptionalUnsigned32(object, QStringLiteral("from_as"), &event.fromAs, error) ||
        !readOptionalUnsigned32(object, QStringLiteral("to_as"), &event.toAs, error) ||
        !readString(object, QStringLiteral("msg_type"), &event.messageType, error) ||
        !readString(object, QStringLiteral("action"), &event.action, error) ||
        !readUnsigned64(object, QStringLiteral("sequence"), &event.sequence, error) ||
        !readStringList(object, QStringLiteral("prefixes"), &event.prefixes, error) ||
        !readStringList(object, QStringLiteral("withdrawn"), &event.withdrawn, error) ||
        !readString(object, QStringLiteral("next_hop"), &event.nextHop, error) || !readAsPath(object, &event.asPath, error) ||
        !readOptionalUnsigned32(object, QStringLiteral("local_pref"), &event.localPref, error) ||
        !readOptionalUnsigned32(object, QStringLiteral("med"), &event.med, error) || !readDetails(object, &event.details, error))
    {
        return std::nullopt;
    }

    event.timestamp = QDateTime::fromString(timestamp, QString::fromLatin1(TimestampFormat));
    if (!event.timestamp.isValid())
    {
        setError(error, QStringLiteral("SimulationEvent JSON 字段 timestamp 不是有效的 wall-clock 时间"));
        return std::nullopt;
    }
    return event;
}

} // namespace bgptester
