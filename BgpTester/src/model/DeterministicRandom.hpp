#pragma once

#include <QByteArrayView>
#include <QtGlobal>

#include <algorithm>

namespace bgptester
{

// Version 1 of the topology-generation PRNG. Its state transition and bounded
// mapping are implemented here instead of delegated to the C++ standard
// library, whose distributions are not required to match across platforms.
class DeterministicRandom final
{
public:
    static constexpr int AlgorithmVersion = 1;

    explicit DeterministicRandom(quint64 seed) : state_(seed)
    {
    }

    static quint64 seedFromBytes(QByteArrayView bytes) noexcept
    {
        quint64 hash = 0xcbf29ce484222325ULL;
        for (const auto byte : bytes)
        {
            hash = (hash ^ static_cast<quint8>(byte)) * 0x100000001b3ULL;
        }
        return hash;
    }

    quint32 bounded(quint32 upperExclusive)
    {
        if (upperExclusive <= 1)
        {
            return 0;
        }
        const auto rejectionThreshold = static_cast<quint32>(-upperExclusive) % upperExclusive;
        while (true)
        {
            const auto value = next32();
            if (value >= rejectionThreshold)
            {
                return value % upperExclusive;
            }
        }
    }

    int boundedInclusive(int minimum, int maximum)
    {
        const auto lower = static_cast<qint64>(std::min(minimum, maximum));
        const auto upper = static_cast<qint64>(std::max(minimum, maximum));
        const auto count = static_cast<quint64>(upper - lower) + 1U;
        const auto offset = count == (quint64{1} << 32U) ? next32() : bounded(static_cast<quint32>(count));
        return static_cast<int>(lower + offset);
    }

private:
    quint32 next32()
    {
        state_ += 0x9e3779b97f4a7c15ULL;
        auto value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<quint32>(value >> 32U);
    }

    quint64 state_ = 0;
};

} // namespace bgptester
