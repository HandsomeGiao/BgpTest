#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QStringView>
#include <QtGlobal>

#include <bit>
#include <cmath>

namespace bgptester
{

// A versioned semantic encoding for data that participates in deterministic
// hashes or random seeds. It deliberately does not depend on Qt's JSON writer:
// object keys are sorted, sizes and UTF-16 code units use little endian, and
// numbers use normalized IEEE-754 bits. This is not a storage format.
inline constexpr int CanonicalJsonEncodingVersion = 1;

namespace canonical_json_detail
{

inline void appendUnsigned(QByteArray& output, quint64 value)
{
    for (int byte = 0; byte < 8; ++byte)
    {
        output.append(static_cast<char>((value >> (byte * 8)) & 0xffU));
    }
}

inline void appendString(QByteArray& output, QStringView value)
{
    appendUnsigned(output, static_cast<quint64>(value.size()));
    for (const auto codeUnit : value)
    {
        const auto value16 = codeUnit.unicode();
        output.append(static_cast<char>(value16 & 0xffU));
        output.append(static_cast<char>((value16 >> 8U) & 0xffU));
    }
}

inline void appendValue(QByteArray& output, const QJsonValue& value)
{
    switch (value.type())
    {
        case QJsonValue::Undefined:
            output.append('U');
            return;
        case QJsonValue::Null:
            output.append('N');
            return;
        case QJsonValue::Bool:
            output.append('B');
            output.append(value.toBool() ? '\1' : '\0');
            return;
        case QJsonValue::Double:
        {
            output.append('D');
            auto number = value.toDouble();
            // JSON has only one zero and no NaN payload semantics. Normalizing
            // them prevents host/parser representation details entering a seed.
            if (number == 0.0)
            {
                number = 0.0;
            }
            const auto bits = std::isnan(number) ? quint64{0x7ff8000000000000ULL} : std::bit_cast<quint64>(number);
            appendUnsigned(output, bits);
            return;
        }
        case QJsonValue::String:
            output.append('S');
            appendString(output, value.toString());
            return;
        case QJsonValue::Array:
        {
            output.append('A');
            const auto array = value.toArray();
            appendUnsigned(output, static_cast<quint64>(array.size()));
            for (const auto& entry : array)
            {
                appendValue(output, entry);
            }
            return;
        }
        case QJsonValue::Object:
        {
            output.append('O');
            const auto object = value.toObject();
            auto keys = object.keys();
            keys.sort(Qt::CaseSensitive);
            appendUnsigned(output, static_cast<quint64>(keys.size()));
            for (const auto& key : keys)
            {
                appendString(output, key);
                appendValue(output, object.value(key));
            }
            return;
        }
    }
}

} // namespace canonical_json_detail

inline QByteArray canonicalJsonEncoding(const QJsonValue& value)
{
    static_assert(sizeof(double) == sizeof(quint64));
    QByteArray output("BCJ", 3);
    output.append(static_cast<char>(CanonicalJsonEncodingVersion));
    canonical_json_detail::appendValue(output, value);
    return output;
}

inline QByteArray canonicalJsonEncoding(const QJsonObject& object)
{
    return canonicalJsonEncoding(QJsonValue(object));
}

inline QByteArray canonicalJsonEncoding(const QJsonArray& array)
{
    return canonicalJsonEncoding(QJsonValue(array));
}

} // namespace bgptester
