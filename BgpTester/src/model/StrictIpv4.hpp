#pragma once

#include <QStringView>
#include <QtGlobal>

namespace bgptester
{

namespace strict_ipv4_detail
{

inline bool parseDecimal(QStringView text, int maximum, int* parsed = nullptr)
{
    if (text.isEmpty() || (text.size() > 1 && text.front() == u'0'))
    {
        return false;
    }
    int value = 0;
    for (const auto character : text)
    {
        const auto code = character.unicode();
        if (code < u'0' || code > u'9')
        {
            return false;
        }
        const auto digit = static_cast<int>(code - u'0');
        if (value > (maximum - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (parsed)
    {
        *parsed = value;
    }
    return true;
}

} // namespace strict_ipv4_detail

// Accept only the canonical four-component ASCII decimal representation. This
// avoids delegating acceptance of shortened, octal-looking, or locale-shaped
// addresses to a Qt/network-stack parser whose compatibility rules may vary.
inline bool isCanonicalIpv4Address(QStringView text, bool allowZero = true)
{
    quint32 address = 0;
    qsizetype componentStart = 0;
    int componentCount = 0;
    for (qsizetype index = 0; index <= text.size(); ++index)
    {
        if (index != text.size() && text.at(index) != u'.')
        {
            continue;
        }
        if (componentCount >= 4)
        {
            return false;
        }
        int component = 0;
        if (!strict_ipv4_detail::parseDecimal(text.sliced(componentStart, index - componentStart), 255, &component))
        {
            return false;
        }
        address = (address << 8U) | static_cast<quint32>(component);
        ++componentCount;
        componentStart = index + 1;
    }
    return componentCount == 4 && (allowZero || address != 0);
}

inline bool isCanonicalIpv4Prefix(QStringView text)
{
    const auto slash = text.lastIndexOf(u'/');
    if (slash <= 0 || slash == text.size() - 1 || text.first(slash).contains(u'/'))
    {
        return false;
    }
    return strict_ipv4_detail::parseDecimal(text.sliced(slash + 1), 32) &&
           isCanonicalIpv4Address(text.first(slash));
}

} // namespace bgptester
