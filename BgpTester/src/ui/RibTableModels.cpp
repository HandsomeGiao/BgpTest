#include "ui/RibTableModels.hpp"

#include <QStringList>

#include <algorithm>
#include <tuple>

namespace bgptester
{
namespace
{

QString asPathText(const QVector<quint32>& path)
{
    QStringList values;
    values.reserve(path.size());
    for (const auto asn : path)
    {
        values.append(QString::number(asn));
    }
    return values.join(u' ');
}

QVariant routeData(const QString& prefix, const RouteEntry& route, int column, bool includeRibColumns, const QString& rib = {},
                   bool best = false)
{
    if (includeRibColumns)
    {
        switch (column)
        {
            case AllRoutesTableModel::Prefix:
                return prefix;
            case AllRoutesTableModel::Rib:
                return rib;
            case AllRoutesTableModel::Source:
                return route.localOrigin ? QStringLiteral("local") : route.learnedFrom;
            case AllRoutesTableModel::Best:
                return best ? QStringLiteral("✓") : QString{};
            case AllRoutesTableModel::NextHop:
                return route.attributes.nextHop;
            case AllRoutesTableModel::AsPath:
                return asPathText(route.attributes.asPath);
            case AllRoutesTableModel::LocalPref:
                return route.attributes.localPref;
            case AllRoutesTableModel::Med:
                return route.attributes.med;
            default:
                return {};
        }
    }

    switch (column)
    {
        case BestRoutesTableModel::Prefix:
            return prefix;
        case BestRoutesTableModel::Source:
            return route.localOrigin ? QStringLiteral("local") : route.learnedFrom;
        case BestRoutesTableModel::Session:
            return toString(route.sourceSession);
        case BestRoutesTableModel::NextHop:
            return route.attributes.nextHop;
        case BestRoutesTableModel::AsPath:
            return asPathText(route.attributes.asPath);
        case BestRoutesTableModel::LocalPref:
            return route.attributes.localPref;
        case BestRoutesTableModel::Med:
            return route.attributes.med;
        default:
            return {};
    }
}

} // namespace

BestRoutesTableModel::BestRoutesTableModel(QObject* parent) : QAbstractTableModel(parent)
{
}

int BestRoutesTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int BestRoutesTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant BestRoutesTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
    {
        return {};
    }
    if (role == Qt::TextAlignmentRole)
    {
        return QVariant::fromValue(index.column() == Prefix || index.column() == AsPath ? Qt::AlignLeft | Qt::AlignVCenter
                                                                                        : Qt::AlignCenter);
    }
    const auto& row = rows_.at(index.row());
    return role == Qt::DisplayRole ? routeData(row.prefix, *row.route, index.column(), false) : QVariant{};
}

QVariant BestRoutesTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{QStringLiteral("前缀"),     QStringLiteral("来源"),    QStringLiteral("会话"),
                                     QStringLiteral("NEXT_HOP"), QStringLiteral("AS_PATH"), QStringLiteral("LOCAL_PREF"),
                                     QStringLiteral("MED")};
    return headers.value(section);
}

void BestRoutesTableModel::setSnapshot(const RibSnapshot& snapshot)
{
    beginResetModel();
    rows_.clear();
    snapshot_ = snapshot;
    rows_.reserve(snapshot_.locRib.size());
    for (auto it = snapshot_.locRib.cbegin(); it != snapshot_.locRib.cend(); ++it)
    {
        rows_.append(Row{.prefix = it.key(), .route = &it.value()});
    }
    std::sort(rows_.begin(), rows_.end(), [](const Row& lhs, const Row& rhs) { return lhs.prefix < rhs.prefix; });
    endResetModel();
}

QString BestRoutesTableModel::prefixAt(int row) const
{
    return row >= 0 && row < rows_.size() ? rows_.at(row).prefix : QString{};
}

AllRoutesTableModel::AllRoutesTableModel(QObject* parent) : QAbstractTableModel(parent)
{
}

int AllRoutesTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int AllRoutesTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AllRoutesTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
    {
        return {};
    }
    if (role == Qt::TextAlignmentRole)
    {
        return QVariant::fromValue(index.column() == Prefix || index.column() == AsPath ? Qt::AlignLeft | Qt::AlignVCenter
                                                                                        : Qt::AlignCenter);
    }
    const auto& row = rows_.at(index.row());
    return role == Qt::DisplayRole ? routeData(row.prefix, *row.route, index.column(), true, row.rib, row.best) : QVariant{};
}

QVariant AllRoutesTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{QStringLiteral("前缀"),       QStringLiteral("RIB"),      QStringLiteral("来源"),
                                     QStringLiteral("最佳"),       QStringLiteral("NEXT_HOP"), QStringLiteral("AS_PATH"),
                                     QStringLiteral("LOCAL_PREF"), QStringLiteral("MED")};
    return headers.value(section);
}

void AllRoutesTableModel::setSnapshot(const RibSnapshot& snapshot)
{
    beginResetModel();
    rows_.clear();
    snapshot_ = snapshot;
    qsizetype rowCount = snapshot_.localRoutes.size();
    for (const auto& routes : snapshot_.adjRibIn)
    {
        rowCount += routes.size();
    }
    rows_.reserve(rowCount);

    const auto appendRoute = [this](QString prefix, QString rib, const RouteEntry& route)
    {
        const auto best = snapshot_.locRib.constFind(prefix);
        rows_.append(Row{.prefix = std::move(prefix),
                         .rib = std::move(rib),
                         .route = &route,
                         .best = best != snapshot_.locRib.cend() && best.value() == route});
    };
    for (auto it = snapshot_.localRoutes.cbegin(); it != snapshot_.localRoutes.cend(); ++it)
    {
        appendRoute(it.key(), QStringLiteral("Local"), it.value());
    }
    for (auto peer = snapshot_.adjRibIn.cbegin(); peer != snapshot_.adjRibIn.cend(); ++peer)
    {
        const auto rib = QStringLiteral("Adj-In/%1").arg(peer.key());
        for (auto route = peer->cbegin(); route != peer->cend(); ++route)
        {
            appendRoute(route.key(), rib, route.value());
        }
    }
    std::sort(rows_.begin(), rows_.end(),
              [](const Row& lhs, const Row& rhs) { return std::tie(lhs.prefix, lhs.rib) < std::tie(rhs.prefix, rhs.rib); });
    endResetModel();
}

} // namespace bgptester
