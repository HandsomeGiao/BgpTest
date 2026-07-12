#include "ui/EventTableModel.hpp"

#include <QBrush>
#include <QColor>

namespace bgptester
{
namespace
{

QString asPathText(const QVector<quint32>& path)
{
    QStringList values;
    for (const auto value : path)
    {
        values.append(QString::number(value));
    }
    return values.join(u' ');
}

} // namespace

EventTableModel::EventTableModel(QObject* parent) : QAbstractTableModel(parent)
{
}

int EventTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : events_.size();
}

int EventTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant EventTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size())
    {
        return {};
    }
    const auto& event = events_.at(index.row());
    if (role == Qt::TextAlignmentRole)
    {
        return index.column() == Prefixes || index.column() == Withdrawn || index.column() == AsPath
                   ? QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter)
                   : QVariant::fromValue(Qt::AlignCenter);
    }
    if (role == Qt::ForegroundRole)
    {
        if (event.action == QStringLiteral("WITHDRAW"))
        {
            return QBrush(QColor(QStringLiteral("#d1495b")));
        }
        if (event.action == QStringLiteral("UPDATE"))
        {
            return QBrush(QColor(QStringLiteral("#16835d")));
        }
        if (event.action == QStringLiteral("TOPOLOGY"))
        {
            return QBrush(QColor(QStringLiteral("#8055a6")));
        }
    }
    if (role != Qt::DisplayRole)
    {
        return {};
    }
    switch (index.column())
    {
        case Id:
            return QVariant::fromValue<qulonglong>(event.id);
        case Time:
            return event.timestamp.toString(QStringLiteral("HH:mm:ss.zzz"));
        case Event:
            return event.event;
        case Router:
            return event.router;
        case From:
            return event.from;
        case To:
            return event.to;
        case FromAs:
            return event.fromAs ? QVariant::fromValue(*event.fromAs) : QVariant{};
        case ToAs:
            return event.toAs ? QVariant::fromValue(*event.toAs) : QVariant{};
        case MessageType:
            return event.messageType;
        case Action:
            return event.action;
        case Sequence:
            return event.sequence == 0 ? QVariant{} : QVariant::fromValue<qulonglong>(event.sequence);
        case Prefixes:
            return event.prefixes.join(QStringLiteral(", "));
        case Withdrawn:
            return event.withdrawn.join(QStringLiteral(", "));
        case NextHop:
            return event.nextHop;
        case AsPath:
            return asPathText(event.asPath);
        case LocalPref:
            return event.localPref ? QVariant::fromValue(*event.localPref) : QVariant{};
        case Med:
            return event.med ? QVariant::fromValue(*event.med) : QVariant{};
        default:
            return {};
    }
}

QVariant EventTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{
        QStringLiteral("ID"),   QStringLiteral("时间"),     QStringLiteral("事件"),    QStringLiteral("路由器"),
        QStringLiteral("来源"), QStringLiteral("目的"),     QStringLiteral("来源 AS"), QStringLiteral("目的 AS"),
        QStringLiteral("报文"), QStringLiteral("动作"),     QStringLiteral("序号"),    QStringLiteral("前缀"),
        QStringLiteral("撤销"), QStringLiteral("NEXT_HOP"), QStringLiteral("AS_PATH"), QStringLiteral("LOCAL_PREF"),
        QStringLiteral("MED"),
    };
    return headers.value(section);
}

void EventTableModel::appendEvent(const SimulationEvent& event)
{
    if (events_.size() >= capacity_)
    {
        const auto removeCount = events_.size() - capacity_ + 1;
        beginRemoveRows({}, 0, removeCount - 1);
        events_.remove(0, removeCount);
        endRemoveRows();
    }
    const auto row = events_.size();
    beginInsertRows({}, row, row);
    events_.append(event);
    endInsertRows();
}

void EventTableModel::setEvents(QVector<SimulationEvent> events)
{
    beginResetModel();
    if (events.size() > capacity_)
    {
        events.remove(0, events.size() - capacity_);
    }
    events_ = std::move(events);
    endResetModel();
}

void EventTableModel::clear()
{
    if (events_.isEmpty())
    {
        return;
    }
    beginResetModel();
    events_.clear();
    endResetModel();
}

const SimulationEvent* EventTableModel::eventAt(int row) const
{
    return row >= 0 && row < events_.size() ? &events_.at(row) : nullptr;
}

} // namespace bgptester
