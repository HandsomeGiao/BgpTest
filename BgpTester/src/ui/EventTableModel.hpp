#pragma once

#include "engine/BgpTypes.hpp"

#include <QAbstractTableModel>
#include <QVector>

namespace bgptester
{

class EventTableModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        Id,
        Time,
        Event,
        Router,
        From,
        To,
        FromAs,
        ToAs,
        MessageType,
        Action,
        Sequence,
        Prefixes,
        Withdrawn,
        NextHop,
        AsPath,
        LocalPref,
        Med,
        ColumnCount
    };

    explicit EventTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void appendEvent(const SimulationEvent& event);
    void appendEvents(QVector<SimulationEvent> events);
    void setEvents(QVector<SimulationEvent> events);
    void clear();
    const SimulationEvent* eventAt(int row) const;
    static bool matchesFilter(const SimulationEvent& event, const QString& filter);
    static constexpr qsizetype liveCapacity()
    {
        return capacity_;
    }

private:
    QVector<SimulationEvent> events_;
    static constexpr qsizetype capacity_ = 20000;
};

} // namespace bgptester
