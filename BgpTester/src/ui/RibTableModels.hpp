#pragma once

#include "engine/BgpTypes.hpp"

#include <QAbstractTableModel>
#include <QVector>

namespace bgptester
{

class BestRoutesTableModel final : public QAbstractTableModel
{
public:
    enum Column
    {
        Prefix,
        Source,
        Session,
        NextHop,
        AsPath,
        LocalPref,
        Med,
        ColumnCount
    };

    explicit BestRoutesTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setSnapshot(const RibSnapshot& snapshot);
    QString prefixAt(int row) const;

private:
    struct Row
    {
        QString prefix;
        const RouteEntry* route = nullptr;
    };

    RibSnapshot snapshot_;
    QVector<Row> rows_;
};

class AllRoutesTableModel final : public QAbstractTableModel
{
public:
    enum Column
    {
        Prefix,
        Rib,
        Source,
        Best,
        NextHop,
        AsPath,
        LocalPref,
        Med,
        ColumnCount
    };

    explicit AllRoutesTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setSnapshot(const RibSnapshot& snapshot);

private:
    struct Row
    {
        QString prefix;
        QString rib;
        const RouteEntry* route = nullptr;
        bool best = false;
    };

    RibSnapshot snapshot_;
    QVector<Row> rows_;
};

} // namespace bgptester
