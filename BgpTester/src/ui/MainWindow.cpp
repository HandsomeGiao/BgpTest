#include "ui/MainWindow.hpp"

#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "ui/Dialogs.hpp"
#include "ui/EventTableModel.hpp"
#include "ui/RibTableModels.hpp"
#include "ui/TopologyScene.hpp"

#include <QAbstractListModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QProgressDialog>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>

namespace bgptester
{
namespace
{

struct HistoryLoadState
{
    EventHistoryPage page;
    ConvergenceHistoryPage convergencePage;
    QString error;
    QString convergenceError;
    std::atomic<qint64> loadedRows{0};
    std::atomic<qint64> totalRows{0};
    std::atomic_bool cancelRequested{false};
};

struct TopologyLoadState
{
    std::optional<Topology> topology;
    QString error;
    std::atomic<qint64> bytesProcessed{0};
    std::atomic<qint64> totalBytes{0};
    std::atomic<qint64> routersLoaded{0};
    std::atomic<qint64> linksLoaded{0};
    std::atomic<int> stage{static_cast<int>(TopologyLoadStage::ReadingRouters)};
    std::atomic_bool cancelRequested{false};
};

struct LiveCountQueryState
{
    EventHistoryPage page;
    QString error;
};

constexpr int convergenceHistoryCapacity = 5000;

class PersistentCheckableMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        auto* action = actionAt(event->position().toPoint());
        if (action && action->isEnabled() && action->isCheckable())
        {
            pressedCheckableAction_ = action;
            setActiveAction(action);
            event->accept();
            return;
        }
        pressedCheckableAction_ = nullptr;
        QMenu::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        auto* action = actionAt(event->position().toPoint());
        if (pressedCheckableAction_)
        {
            if (action == pressedCheckableAction_ && action->isEnabled())
            {
                action->setChecked(!action->isChecked());
            }
            pressedCheckableAction_ = nullptr;
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        auto* action = activeAction();
        if (action && action->isEnabled() && action->isCheckable() &&
            (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
        {
            action->setChecked(!action->isChecked());
            event->accept();
            return;
        }
        QMenu::keyPressEvent(event);
    }

private:
    QAction* pressedCheckableAction_ = nullptr;
};

QTableWidgetItem* tableItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString relationshipDisplayName(NeighborRelationship relationship)
{
    switch (relationship)
    {
        case NeighborRelationship::Peer:
            return QStringLiteral("Peer");
        case NeighborRelationship::Provider:
            return QStringLiteral("Provider");
        case NeighborRelationship::Customer:
            return QStringLiteral("Customer");
        case NeighborRelationship::Unspecified:
            return QStringLiteral("未指定");
    }
    return QStringLiteral("未指定");
}

void configureDataTable(QTableWidget* table)
{
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

void configureDataView(QTableView* table)
{
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
}

void setDisplayedShortcut(QAction* action, const QKeySequence& shortcut)
{
    action->setShortcut(shortcut);
    action->setShortcutVisibleInContextMenu(true);

    const auto shortcutText = shortcut.toString(QKeySequence::NativeText);
    if (!shortcutText.isEmpty())
    {
        action->setToolTip(QStringLiteral("%1（%2）").arg(action->text(), shortcutText));
    }
}

void showShortcutOnToolbarButton(QToolBar* toolbar, QAction* action)
{
    const auto shortcutText = action->shortcut().toString(QKeySequence::NativeText);
    const auto toolbarText = QStringLiteral("%1 [%2]").arg(action->text(), shortcutText);
    action->setIconText(toolbarText);

    auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action));
    if (!button)
    {
        return;
    }

    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setText(toolbarText);
}

std::optional<qint64> nonNegativeDetail(const SimulationEvent& event, const QString& key)
{
    bool ok = false;
    const auto value = event.details.value(key).toLongLong(&ok);
    return ok && value >= 0 ? std::optional<qint64>{value} : std::nullopt;
}

std::optional<quint64> positiveDetail(const SimulationEvent& event, const QString& key)
{
    bool ok = false;
    const auto value = event.details.value(key).toULongLong(&ok);
    return ok && value > 0 ? std::optional<quint64>{value} : std::nullopt;
}

std::optional<quint64> unsignedDetail(const SimulationEvent& event, const QString& key)
{
    bool ok = false;
    const auto value = event.details.value(key).toULongLong(&ok);
    return ok ? std::optional<quint64>{value} : std::nullopt;
}

QString durationText(qint64 durationMs)
{
    if (durationMs < 1000)
    {
        return QStringLiteral("%1 ms").arg(durationMs);
    }
    if (durationMs < 60000)
    {
        return QStringLiteral("%1 s").arg(durationMs / 1000.0, 0, 'f', 3);
    }
    const auto minutes = durationMs / 60000;
    const auto remainingMs = durationMs % 60000;
    return QStringLiteral("%1 min %2 s").arg(minutes).arg(remainingMs / 1000.0, 0, 'f', 3);
}

QString convergenceEventName(const QString& event)
{
    if (event == QStringLiteral("simulation_started"))
    {
        return QStringLiteral("仿真启动");
    }
    if (event == QStringLiteral("link_up"))
    {
        return QStringLiteral("链路恢复");
    }
    if (event == QStringLiteral("link_down"))
    {
        return QStringLiteral("链路断开");
    }
    if (event == QStringLiteral("router_up"))
    {
        return QStringLiteral("节点恢复");
    }
    if (event == QStringLiteral("router_down"))
    {
        return QStringLiteral("节点关闭");
    }
    if (event == QStringLiteral("prefix_advertised"))
    {
        return QStringLiteral("前缀发布");
    }
    if (event == QStringLiteral("prefix_withdrawn"))
    {
        return QStringLiteral("前缀撤销");
    }
    if (event == QStringLiteral("message_received"))
    {
        return QStringLiteral("BGP 报文");
    }
    if (event == QStringLiteral("routing_activity"))
    {
        return QStringLiteral("路由活动");
    }
    return event.isEmpty() ? QStringLiteral("未知事件") : event;
}

QString convergenceEventContext(const SimulationEvent& event)
{
    if (event.event == QStringLiteral("simulation_started"))
    {
        return event.details.value(QStringLiteral("name"));
    }
    if (event.event == QStringLiteral("link_up") || event.event == QStringLiteral("link_down"))
    {
        const auto a = event.details.value(QStringLiteral("a"), event.from);
        const auto b = event.details.value(QStringLiteral("b"), event.to);
        return a.isEmpty() || b.isEmpty() ? QString{} : QStringLiteral("%1 ↔ %2").arg(a, b);
    }
    if (event.event == QStringLiteral("prefix_advertised") || event.event == QStringLiteral("prefix_withdrawn"))
    {
        const auto router = event.details.value(QStringLiteral("router"), event.router);
        const auto prefix = event.details.value(QStringLiteral("prefix"));
        return router.isEmpty() || prefix.isEmpty() ? router : QStringLiteral("%1 · %2").arg(router, prefix);
    }
    if (event.event == QStringLiteral("router_up") || event.event == QStringLiteral("router_down"))
    {
        return event.details.value(QStringLiteral("router"), event.router);
    }
    if (!event.from.isEmpty() && !event.to.isEmpty())
    {
        return QStringLiteral("%1 → %2").arg(event.from, event.to);
    }
    return event.router;
}

QString convergenceTriggerText(const QString& event, const QString& context)
{
    const auto name = convergenceEventName(event);
    return context.isEmpty() ? name : QStringLiteral("%1：%2").arg(name, context);
}

} // namespace

class TopologyRouterListModel final : public QAbstractListModel
{
public:
    explicit TopologyRouterListModel(QObject* parent = nullptr) : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        if (parent.isValid())
        {
            return 0;
        }
        return static_cast<int>(std::min<qsizetype>(routerIds_.size(), std::numeric_limits<int>::max()));
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0)
        {
            return {};
        }
        const auto row = static_cast<qsizetype>(index.row());
        if (row >= routerIds_.size())
        {
            return {};
        }
        if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole)
        {
            return routerIds_[row];
        }
        return {};
    }

    void setTopology(const Topology* topology)
    {
        beginResetModel();
        routerIds_ = topology ? topology->routers.keys() : QStringList{};
        endResetModel();
    }

private:
    QStringList routerIds_;
};

class TopologyLinkListModel final : public QAbstractListModel
{
public:
    explicit TopologyLinkListModel(QObject* parent = nullptr) : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        if (parent.isValid() || !topology_)
        {
            return 0;
        }
        return static_cast<int>(std::min<qsizetype>(topology_->links.size(), std::numeric_limits<int>::max()));
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!topology_ || !index.isValid() || index.row() < 0)
        {
            return {};
        }
        const auto row = static_cast<qsizetype>(index.row());
        if (row >= topology_->links.size())
        {
            return {};
        }
        const auto& link = topology_->links[row];
        if (role == Qt::DisplayRole || role == Qt::EditRole)
        {
            return QStringLiteral("%1 ↔ %2").arg(link.a, link.b);
        }
        if (role == Qt::UserRole)
        {
            return Topology::edgeKey(link.a, link.b);
        }
        if (role == Qt::UserRole + 1)
        {
            return link.a;
        }
        if (role == Qt::UserRole + 2)
        {
            return link.b;
        }
        return {};
    }

    void setTopology(const Topology* topology)
    {
        beginResetModel();
        topology_ = topology;
        endResetModel();
    }

private:
    const Topology* topology_ = nullptr;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setObjectName(QStringLiteral("BgpTesterMainWindow"));
    resize(1480, 900);
    setDockNestingEnabled(true);

    scene_ = new TopologyScene(this);
    view_ = new TopologyView(scene_, this);
    setCentralWidget(view_);
    eventStore_ = new EventStore;
    eventStore_->moveToThread(&eventStoreThread_);
    eventStoreThread_.setObjectName(QStringLiteral("BmpEventStoreThread"));
    connect(&eventStoreThread_, &QThread::finished, eventStore_, &QObject::deleteLater);
    eventStoreThread_.start();
    eventModel_ = new EventTableModel(this);

    buildActions();
    buildMenusAndToolbar();
    buildInspectorDock();
    buildEventDock();
    connectEngine();

    connect(scene_, &TopologyScene::createRouterRequested, this, &MainWindow::createRouter);
    connect(scene_, &TopologyScene::createLinkRequested, this, &MainWindow::createLink);
    connect(scene_, &TopologyScene::editRouterRequested, this, &MainWindow::editRouter);
    connect(scene_, &TopologyScene::editLinkRequested, this, &MainWindow::editLink);
    connect(scene_, &TopologyScene::topologyModified, this, &MainWindow::markDirty);
    connect(scene_, &TopologyScene::selectionContextChanged, this, &MainWindow::sceneSelectionChanged);
    connect(eventStore_, &EventStore::eventsStored, this, &MainWindow::enqueueStoredEvents, Qt::QueuedConnection);
    connect(eventStore_, &EventStore::countSnapshotReady, this,
            [this](quint64 requestId, quint64 runSerial, const QString& databasePath, quint64 maxEventId, const QString& error)
            {
                liveCountSnapshotPending_ = false;
                if (requestId != liveCountRequestId_ || runSerial != eventRunSerial_ || !historyDatabasePath_.isEmpty() || closing_ ||
                    simulationStartPending_)
                {
                    if (liveCountRefreshPending_ && !closing_ && historyDatabasePath_.isEmpty() && !eventFilterTimer_->isActive())
                    {
                        liveCountRefreshPending_ = false;
                        QTimer::singleShot(0, this, &MainWindow::refreshEventFilter);
                    }
                    return;
                }
                if (!error.isEmpty())
                {
                    statusBar()->showMessage(error, 5000);
                    eventFilteredCountKnown_ = false;
                    eventCountQueryFailed_ = true;
                    liveCountDeltaActive_ = false;
                    updateEventCountLabel();
                    return;
                }
                liveDeltaEventTotal_ = 0;
                liveDeltaEventFiltered_ = 0;
                liveDeltaMessageTotal_ = 0;
                liveDeltaMessageFiltered_ = 0;
                liveCountDeltaActive_ = true;
                liveCountRefreshPending_ = false;
                startLiveCountQuery(requestId, runSerial, databasePath, maxEventId, eventFilterEdit_->text());
            });
    connect(eventStore_, &EventStore::storeError, this, [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(eventStore_, &EventStore::pathsChanged, this,
            [this](const QString&, const QString& database)
            {
                logPathLabel_->setText(QStringLiteral("日志：%1").arg(QDir::toNativeSeparators(database)));
                logPathLabel_->setToolTip(database);
            });

    uiEventDrainTimer_ = new QTimer(this);
    uiEventDrainTimer_->setSingleShot(true);
    connect(uiEventDrainTimer_, &QTimer::timeout, this, &MainWindow::drainUiEventQueue);

    ribRefreshTimer_ = new QTimer(this);
    ribRefreshTimer_->setSingleShot(true);
    ribRefreshTimer_->setInterval(500);
    connect(ribRefreshTimer_, &QTimer::timeout, this, &MainWindow::requestSelectedRouterSnapshot);

    simulationStatusLabel_ = new QLabel(QStringLiteral("● 已停止"), this);
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#6c757d"));
    statsLabel_ = new QLabel(QStringLiteral("事件 0"), this);
    logPathLabel_ = new QLabel(QStringLiteral("尚未生成日志"), this);
    logPathLabel_->setMinimumWidth(280);
    statusBar()->addPermanentWidget(simulationStatusLabel_);
    statusBar()->addPermanentWidget(statsLabel_);
    statusBar()->addPermanentWidget(logPathLabel_, 1);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("main/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("main/state")).toByteArray());
    setTopology(starterTopology());
    setDirty(false);
    const auto lastTopology = settings.value(QStringLiteral("files/lastTopology")).toString();
    if (!lastTopology.isEmpty() && QFileInfo::exists(lastTopology))
    {
        const auto absolutePath = QFileInfo(lastTopology).absoluteFilePath();
        const auto restoreGeneration = topologyLoadGeneration_;
        QTimer::singleShot(0, this,
                           [this, absolutePath, restoreGeneration]
                           {
                               if (!closing_ && !topologyLoadInProgress_ && topologyLoadGeneration_ == restoreGeneration)
                               {
                                   openTopologyPath(absolutePath, false);
                               }
                           });
    }
}

MainWindow::~MainWindow()
{
    closing_ = true;
    const auto startupWasPending = simulationStartPending_;
    if (startupWasPending)
    {
        engine_->requestStartupCancellation();
    }
    stopTopologyLoad();
    ++historyQueryGeneration_;
    ++liveCountRequestId_;
    stopAndWaitForQueryThreads();
    if (engineThread_.isRunning())
    {
        if (simulationRunning_ || startupWasPending)
        {
            QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::BlockingQueuedConnection);
        }
        engineThread_.quit();
        engineThread_.wait();
    }
    endEventRun(true);
    if (eventStoreThread_.isRunning())
    {
        eventStoreThread_.quit();
        eventStoreThread_.wait();
    }
}

void MainWindow::buildActions()
{
    newAction_ = new QAction(QStringLiteral("新建"), this);
    setDisplayedShortcut(newAction_, QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, &MainWindow::newTopology);
    openAction_ = new QAction(QStringLiteral("打开拓扑…"), this);
    setDisplayedShortcut(openAction_, QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::openTopology);
    saveAction_ = new QAction(QStringLiteral("保存"), this);
    setDisplayedShortcut(saveAction_, QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::saveTopology);
    saveAsAction_ = new QAction(QStringLiteral("另存为…"), this);
    setDisplayedShortcut(saveAsAction_, QKeySequence::SaveAs);
    connect(saveAsAction_, &QAction::triggered, this, &MainWindow::saveTopologyAs);
    openHistoryAction_ = new QAction(QStringLiteral("打开 SQLite 日志…"), this);
    connect(openHistoryAction_, &QAction::triggered, this, &MainWindow::openHistory);
    settingsAction_ = new QAction(QStringLiteral("仿真设置…"), this);
    connect(settingsAction_, &QAction::triggered, this, &MainWindow::editSimulationSettings);
    batchTopologyAction_ = new QAction(QStringLiteral("批量配置拓扑…"), this);
    connect(batchTopologyAction_, &QAction::triggered, this, &MainWindow::editTopologyBatchProperties);

    modeGroup_ = new QActionGroup(this);
    modeGroup_->setExclusive(true);
    selectModeAction_ = new QAction(QStringLiteral("选择/移动"), this);
    selectModeAction_->setCheckable(true);
    selectModeAction_->setChecked(true);
    setDisplayedShortcut(selectModeAction_, QKeySequence(Qt::Key_V));
    addRouterAction_ = new QAction(QStringLiteral("添加路由器"), this);
    addRouterAction_->setCheckable(true);
    setDisplayedShortcut(addRouterAction_, QKeySequence(Qt::Key_R));
    addLinkAction_ = new QAction(QStringLiteral("添加链路"), this);
    addLinkAction_->setCheckable(true);
    setDisplayedShortcut(addLinkAction_, QKeySequence(Qt::Key_Q));
    modeGroup_->addAction(selectModeAction_);
    modeGroup_->addAction(addRouterAction_);
    modeGroup_->addAction(addLinkAction_);
    connect(selectModeAction_, &QAction::triggered, this, [this] { scene_->setMode(TopologyScene::Mode::Select); });
    connect(addRouterAction_, &QAction::triggered, this, [this] { scene_->setMode(TopologyScene::Mode::AddRouter); });
    connect(addLinkAction_, &QAction::triggered, this, [this] { scene_->setMode(TopologyScene::Mode::AddLink); });
    deleteAction_ = new QAction(QStringLiteral("删除所选"), this);
    setDisplayedShortcut(deleteAction_, QKeySequence::Delete);
    connect(deleteAction_, &QAction::triggered, this, &MainWindow::deleteSelection);
    fitAction_ = new QAction(QStringLiteral("适合窗口"), this);
    setDisplayedShortcut(fitAction_, QKeySequence(Qt::Key_F));
    connect(fitAction_, &QAction::triggered, this,
            [this]
            {
                if (!scene_->items().isEmpty())
                {
                    view_->fitInView(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
                }
            });

    startAction_ = new QAction(QStringLiteral("启动仿真"), this);
    setDisplayedShortcut(startAction_, QKeySequence(Qt::Key_F5));
    connect(startAction_, &QAction::triggered, this, &MainWindow::startSimulation);
    stopAction_ = new QAction(QStringLiteral("停止"), this);
    setDisplayedShortcut(stopAction_, QKeySequence(Qt::SHIFT | Qt::Key_F5));
    stopAction_->setEnabled(false);
    connect(stopAction_, &QAction::triggered, this, &MainWindow::stopSimulation);
}

void MainWindow::buildMenusAndToolbar()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addActions({newAction_, openAction_, saveAction_, saveAsAction_});
    fileMenu->addSeparator();
    fileMenu->addAction(openHistoryAction_);
    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction(QStringLiteral("退出"));
    setDisplayedShortcut(exitAction, QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    auto* editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    editMenu->addActions({selectModeAction_, addRouterAction_, addLinkAction_, deleteAction_});
    editMenu->addSeparator();
    editMenu->addAction(batchTopologyAction_);
    editMenu->addAction(settingsAction_);
    auto* simulationMenu = menuBar()->addMenu(QStringLiteral("仿真(&S)"));
    simulationMenu->addActions({startAction_, stopAction_});
    viewMenu_ = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu_->addAction(fitAction_);
    viewMenu_->addSeparator();

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->addActions({newAction_, openAction_, saveAction_});
    toolbar->addSeparator();
    toolbar->addActions({selectModeAction_, addRouterAction_, addLinkAction_, deleteAction_});
    toolbar->addAction(fitAction_);
    for (auto* action : {selectModeAction_, addRouterAction_, addLinkAction_, deleteAction_, fitAction_})
    {
        showShortcutOnToolbarButton(toolbar, action);
    }
    toolbar->addSeparator();
    toolbar->addActions({startAction_, stopAction_});
    toolbar->addSeparator();
    toolbar->addAction(batchTopologyAction_);
    toolbar->addAction(settingsAction_);
}

void MainWindow::buildInspectorDock()
{
    auto* dock = new QDockWidget(QStringLiteral("路由检查器"), this);
    dock->setObjectName(QStringLiteral("inspectorDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* container = new QWidget(dock);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* routerRow = new QHBoxLayout;
    routerRow->addWidget(new QLabel(QStringLiteral("路由器"), container));
    routerCombo_ = new QComboBox(container);
    routerListModel_ = new TopologyRouterListModel(this);
    routerCombo_->setModel(routerListModel_);
    routerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    routerCombo_->setMinimumContentsLength(18);
    routerCombo_->setMaxVisibleItems(30);
    if (auto* list = qobject_cast<QListView*>(routerCombo_->view()))
    {
        list->setUniformItemSizes(true);
    }
    routerRow->addWidget(routerCombo_, 1);
    connect(routerCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::selectedRouterChanged);
    layout->addLayout(routerRow);

    inspectorTabs_ = new QTabWidget(container);
    auto* tabs = inspectorTabs_;
    ribTable_ = new QTableView(tabs);
    bestRoutesModel_ = new BestRoutesTableModel(this);
    ribTable_->setModel(bestRoutesModel_);
    configureDataView(ribTable_);
    connect(ribTable_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { highlightSelectedRoute(); });
    tabs->addTab(ribTable_, QStringLiteral("最佳路由"));

    allRoutesTable_ = new QTableView(tabs);
    allRoutesModel_ = new AllRoutesTableModel(this);
    allRoutesTable_->setModel(allRoutesModel_);
    configureDataView(allRoutesTable_);
    tabs->addTab(allRoutesTable_, QStringLiteral("全部路径"));

    peerTable_ = new QTableWidget(tabs);
    peerTable_->setColumnCount(7);
    peerTable_->setHorizontalHeaderLabels({QStringLiteral("邻居"), QStringLiteral("Remote AS"), QStringLiteral("会话"),
                                           QStringLiteral("商业关系"), QStringLiteral("状态"), QStringLiteral("RR Client"),
                                           QStringLiteral("MRAI")});
    configureDataTable(peerTable_);
    tabs->addTab(peerTable_, QStringLiteral("邻居"));

    auto* control = new QWidget(tabs);
    auto* controlLayout = new QVBoxLayout(control);
    auto* nodeBox = new QGroupBox(QStringLiteral("节点状态"), control);
    auto* nodeLayout = new QHBoxLayout(nodeBox);
    routerStateLabel_ = new QLabel(QStringLiteral("—"), nodeBox);
    routerToggleButton_ = new QToolButton(nodeBox);
    routerToggleButton_->setText(QStringLiteral("切换"));
    connect(routerToggleButton_, &QToolButton::clicked, this, &MainWindow::toggleSelectedRouter);
    nodeLayout->addWidget(routerStateLabel_, 1);
    nodeLayout->addWidget(routerToggleButton_);
    controlLayout->addWidget(nodeBox);

    auto* linkBox = new QGroupBox(QStringLiteral("链路状态"), control);
    auto* linkLayout = new QVBoxLayout(linkBox);
    linkSearchEdit_ = new QLineEdit(linkBox);
    linkSearchEdit_->setPlaceholderText(QStringLiteral("搜索链路，例如 R1 或 R1 ↔ R2"));
    linkSearchEdit_->setClearButtonEnabled(true);
    linkSearchEdit_->setToolTip(QStringLiteral("输入任一端点名称以筛选链路，清空后显示全部链路"));
    linkCombo_ = new QComboBox(linkBox);
    linkListModel_ = new TopologyLinkListModel(this);
    linkFilterModel_ = new QSortFilterProxyModel(this);
    linkFilterModel_->setSourceModel(linkListModel_);
    linkFilterModel_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    linkFilterModel_->setFilterRole(Qt::DisplayRole);
    linkCombo_->setModel(linkFilterModel_);
    linkCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    linkCombo_->setMinimumContentsLength(24);
    linkCombo_->setMaxVisibleItems(30);
    if (auto* list = qobject_cast<QListView*>(linkCombo_->view()))
    {
        list->setUniformItemSizes(true);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    }
    connect(linkCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::selectedLinkChanged);
    auto* linkSelectRow = new QHBoxLayout;
    linkBrowseButton_ = new QToolButton(linkBox);
    linkBrowseButton_->setText(QStringLiteral("展开列表"));
    linkBrowseButton_->setArrowType(Qt::DownArrow);
    linkBrowseButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    linkBrowseButton_->setToolTip(QStringLiteral("展开链路列表；可拖动右侧滚动条快速浏览"));
    linkBrowseButton_->setAccessibleName(QStringLiteral("浏览所有链路"));
    connect(linkBrowseButton_, &QToolButton::clicked, linkCombo_, &QComboBox::showPopup);
    connect(linkSearchEdit_, &QLineEdit::returnPressed, linkCombo_, &QComboBox::showPopup);
    connect(linkSearchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const auto selectedLink = linkCombo_->currentData().toString();
        linkFilterModel_->setFilterFixedString(text.trimmed());
        const auto selectedIndex = selectedLink.isEmpty() ? -1 : linkCombo_->findData(selectedLink);
        linkCombo_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : (linkCombo_->count() > 0 ? 0 : -1));
        linkBrowseButton_->setEnabled(!closing_ && linkCombo_->count() > 0);
        refreshRuntimeControls();
    });
    linkSelectRow->addWidget(linkCombo_, 1);
    linkSelectRow->addWidget(linkBrowseButton_);
    auto* linkStateRow = new QHBoxLayout;
    linkStateLabel_ = new QLabel(QStringLiteral("—"), linkBox);
    linkToggleButton_ = new QToolButton(linkBox);
    linkToggleButton_->setText(QStringLiteral("切换"));
    connect(linkToggleButton_, &QToolButton::clicked, this, &MainWindow::toggleSelectedLink);
    linkStateRow->addWidget(linkStateLabel_, 1);
    linkStateRow->addWidget(linkToggleButton_);
    linkLayout->addWidget(linkSearchEdit_);
    linkLayout->addLayout(linkSelectRow);
    linkLayout->addLayout(linkStateRow);
    controlLayout->addWidget(linkBox);

    auto* prefixBox = new QGroupBox(QStringLiteral("前缀扰动"), control);
    auto* prefixLayout = new QVBoxLayout(prefixBox);
    prefixEdit_ = new QLineEdit(prefixBox);
    prefixEdit_->setPlaceholderText(QStringLiteral("例如 203.0.113.0/24"));
    auto* prefixButtons = new QHBoxLayout;
    auto* advertise = new QPushButton(QStringLiteral("发布"), prefixBox);
    auto* withdraw = new QPushButton(QStringLiteral("撤销"), prefixBox);
    connect(advertise, &QPushButton::clicked, this, &MainWindow::advertisePrefix);
    connect(withdraw, &QPushButton::clicked, this, &MainWindow::withdrawPrefix);
    prefixButtons->addWidget(advertise);
    prefixButtons->addWidget(withdraw);
    prefixLayout->addWidget(prefixEdit_);
    prefixLayout->addLayout(prefixButtons);
    controlLayout->addWidget(prefixBox);
    controlLayout->addStretch();
    tabs->addTab(control, QStringLiteral("运行控制"));

    connect(tabs, &QTabWidget::currentChanged, this,
            [this]
            {
                if (currentRib_.router == routerCombo_->currentText())
                {
                    populateRibTables(currentRib_);
                }
                scheduleSelectedRouterSnapshot();
            });

    layout->addWidget(tabs, 1);
    dock->setWidget(container);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    viewMenu_->addAction(dock->toggleViewAction());
}

void MainWindow::buildEventDock()
{
    auto* dock = new QDockWidget(QStringLiteral("BMP 监控器"), this);
    dock->setObjectName(QStringLiteral("eventDock"));
    dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto* monitorTabs = new QTabWidget(dock);
    monitorTabs->setObjectName(QStringLiteral("bmpMonitorTabs"));

    auto* eventPage = new QWidget(monitorTabs);
    auto* layout = new QVBoxLayout(eventPage);
    layout->setContentsMargins(5, 5, 5, 5);
    auto* filterRow = new QHBoxLayout;
    eventFilterEdit_ = new QLineEdit(eventPage);
    eventFilterEdit_->setClearButtonEnabled(true);
    eventFilterEdit_->setPlaceholderText(QStringLiteral("过滤所有列：路由器、动作、ASN、前缀…"));
    eventCountLabel_ = new QLabel(QStringLiteral("显示 0 / 共 0 条"), eventPage);
    eventCountLabel_->setMinimumWidth(430);
    followEventsCheck_ = new QCheckBox(QStringLiteral("跟随实时"), eventPage);
    followEventsCheck_->setChecked(true);
    auto* columnsButton = new QToolButton(eventPage);
    columnsButton->setObjectName(QStringLiteral("eventColumnsButton"));
    columnsButton->setText(QStringLiteral("显示列"));
    columnsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    columnsButton->setPopupMode(QToolButton::InstantPopup);
    columnsButton->setToolTip(QStringLiteral("选择 BMP 事件日志中显示的列"));
    auto* clearButton = new QPushButton(QStringLiteral("清空表格"), eventPage);
    historyButton_ = new QPushButton(QStringLiteral("打开历史…"), eventPage);
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearEventTable);
    connect(historyButton_, &QPushButton::clicked, this, &MainWindow::openHistory);
    filterRow->addWidget(eventFilterEdit_, 1);
    filterRow->addWidget(eventCountLabel_);
    filterRow->addWidget(followEventsCheck_);
    filterRow->addWidget(columnsButton);
    filterRow->addWidget(clearButton);
    filterRow->addWidget(historyButton_);
    layout->addLayout(filterRow);

    eventProxy_ = new QSortFilterProxyModel(this);
    eventProxy_->setSourceModel(eventModel_);
    eventProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    eventProxy_->setFilterKeyColumn(-1);
    eventProxy_->setFilterRole(Qt::UserRole);
    eventProxy_->setSortRole(Qt::DisplayRole);
    eventFilterTimer_ = new QTimer(this);
    eventFilterTimer_->setSingleShot(true);
    eventFilterTimer_->setInterval(300);
    connect(eventFilterTimer_, &QTimer::timeout, this, &MainWindow::refreshEventFilter);
    connect(eventFilterEdit_, &QLineEdit::textChanged, this, &MainWindow::onEventFilterChanged);
    connect(eventProxy_, &QAbstractItemModel::rowsInserted, this, &MainWindow::updateEventCountLabel);
    connect(eventProxy_, &QAbstractItemModel::rowsRemoved, this, &MainWindow::updateEventCountLabel);
    connect(eventProxy_, &QAbstractItemModel::modelReset, this, &MainWindow::updateEventCountLabel);
    connect(eventProxy_, &QAbstractItemModel::layoutChanged, this, &MainWindow::updateEventCountLabel);
    eventView_ = new QTableView(eventPage);
    eventView_->setModel(eventProxy_);
    eventView_->setAlternatingRowColors(true);
    eventView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    eventView_->setSelectionMode(QAbstractItemView::SingleSelection);
    eventView_->setSortingEnabled(true);
    eventView_->sortByColumn(EventTableModel::Id, Qt::AscendingOrder);
    eventView_->verticalHeader()->setVisible(false);
    eventView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    eventView_->horizontalHeader()->setDefaultSectionSize(120);
    eventView_->setColumnWidth(EventTableModel::Id, 80);
    eventView_->setColumnWidth(EventTableModel::Time, 110);
    eventView_->setColumnWidth(EventTableModel::Prefixes, 260);
    eventView_->setColumnWidth(EventTableModel::Withdrawn, 260);
    eventView_->horizontalHeader()->setStretchLastSection(false);

    auto* columnsMenu = new PersistentCheckableMenu(columnsButton);
    columnsMenu->setObjectName(QStringLiteral("eventColumnsMenu"));
    columnsButton->setMenu(columnsMenu);
    const auto hiddenColumns = QSettings().value(QStringLiteral("bmp/eventTableHiddenColumns")).toStringList();
    for (auto column = 0; column < eventModel_->columnCount(); ++column)
    {
        const auto title = eventModel_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        auto* action = columnsMenu->addAction(title);
        action->setObjectName(QStringLiteral("eventColumnAction%1").arg(column));
        action->setCheckable(true);
        const auto visible = !hiddenColumns.contains(QString::number(column));
        action->setChecked(visible);
        eventView_->setColumnHidden(column, !visible);
        connect(action, &QAction::toggled, this,
                [this, column](bool checked)
                {
                    eventView_->setColumnHidden(column, !checked);
                    saveEventColumnVisibility();
                });
    }
    columnsMenu->addSeparator();
    auto* showAllColumnsAction = columnsMenu->addAction(QStringLiteral("显示全部列"));
    connect(showAllColumnsAction, &QAction::triggered, this,
            [this, columnsMenu]
            {
                for (auto* action : columnsMenu->actions())
                {
                    if (action->isCheckable())
                    {
                        action->setChecked(true);
                    }
                }
                saveEventColumnVisibility();
            });
    auto* eventHeader = eventView_->horizontalHeader();
    eventHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(eventHeader, &QWidget::customContextMenuRequested, this,
            [eventHeader, columnsMenu](const QPoint& position) { columnsMenu->exec(eventHeader->mapToGlobal(position)); });

    connect(eventView_, &QTableView::doubleClicked, this, &MainWindow::showEventDetails);
    layout->addWidget(eventView_, 1);
    monitorTabs->addTab(eventPage, QStringLiteral("事件日志"));

    auto* convergencePage = new QWidget(monitorTabs);
    auto* convergenceLayout = new QVBoxLayout(convergencePage);
    convergenceLayout->setContentsMargins(5, 5, 5, 5);
    auto* convergenceSummary = new QHBoxLayout;
    convergenceStateLabel_ = new QLabel(QStringLiteral("当前：尚未开始"), convergencePage);
    convergenceStateLabel_->setObjectName(QStringLiteral("convergenceStateLabel"));
    convergenceCountLabel_ = new QLabel(QStringLiteral("已记录 0 次"), convergencePage);
    auto* clearConvergenceButton = new QPushButton(QStringLiteral("清空记录"), convergencePage);
    connect(clearConvergenceButton, &QPushButton::clicked, this,
            [this]
            {
                const auto stateText = simulationRunning_
                                           ? (simulationConverged_ ? QStringLiteral("当前：已收敛") : QStringLiteral("当前：收敛中"))
                                           : QStringLiteral("当前：尚未开始");
                clearConvergenceHistory(stateText);
            });
    convergenceSummary->addWidget(convergenceStateLabel_, 1);
    convergenceSummary->addWidget(convergenceCountLabel_);
    convergenceSummary->addWidget(clearConvergenceButton);
    convergenceLayout->addLayout(convergenceSummary);

    convergenceTable_ = new QTableWidget(0, 6, convergencePage);
    convergenceTable_->setObjectName(QStringLiteral("convergenceHistoryTable"));
    convergenceTable_->setHorizontalHeaderLabels({QStringLiteral("轮次"), QStringLiteral("触发事件"), QStringLiteral("开始时间"),
                                                  QStringLiteral("完成时间"), QStringLiteral("持续时间"), QStringLiteral("BGP 报文数")});
    configureDataTable(convergenceTable_);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    convergenceLayout->addWidget(convergenceTable_, 1);
    auto* convergenceNote =
        new QLabel(QStringLiteral("持续时间包含配置的收敛静默窗口；BGP 报文数严格统计本轮的 message_received 事件。"), convergencePage);
    convergenceNote->setStyleSheet(QStringLiteral("color:#6c757d"));
    convergenceLayout->addWidget(convergenceNote);
    monitorTabs->addTab(convergencePage, QStringLiteral("收敛时间"));

    dock->setWidget(monitorTabs);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    viewMenu_->addAction(dock->toggleViewAction());
    resizeDocks({dock}, {280}, Qt::Vertical);
    updateEventCountLabel();
    updateHistoryControls();
}

void MainWindow::saveEventColumnVisibility() const
{
    if (!eventView_ || !eventModel_)
    {
        return;
    }
    QStringList hiddenColumns;
    for (auto column = 0; column < eventModel_->columnCount(); ++column)
    {
        if (eventView_->isColumnHidden(column))
        {
            hiddenColumns.append(QString::number(column));
        }
    }
    QSettings().setValue(QStringLiteral("bmp/eventTableHiddenColumns"), hiddenColumns);
}

void MainWindow::updateEventCountLabel()
{
    if (!eventCountLabel_ || !eventProxy_)
    {
        return;
    }
    const auto filteredText = eventFilteredCountKnown_ ? QString::number(eventFilteredCount_)
                                                       : (eventCountQueryFailed_ ? QStringLiteral("失败") : QStringLiteral("查询中"));
    const auto filteredMessageText = eventFilteredCountKnown_ ? QString::number(messageFilteredCount_)
                                                              : (eventCountQueryFailed_ ? QStringLiteral("失败")
                                                                                       : QStringLiteral("查询中"));
    eventCountLabel_->setText(QStringLiteral("窗口 %1 · 事件 %2/%3 · 报文 %4/%5")
                                  .arg(eventProxy_->rowCount())
                                  .arg(filteredText)
                                  .arg(eventTotalCount_)
                                  .arg(filteredMessageText)
                                  .arg(messageTotalCount_));
    eventCountLabel_->setToolTip(
        QStringLiteral("格式：窗口行数 · 过滤后事件/全部事件 · 过滤后报文/全部报文。报文严格指 event=message_received；"
                       "窗口仅保留最近 %1 条，四个计数针对完整日志。")
            .arg(EventTableModel::liveCapacity()));
}

void MainWindow::updateHistoryControls()
{
    const auto enabled = !closing_ && !simulationRunning_ && !simulationStartPending_ && !historyLoadInProgress_;
    if (openHistoryAction_)
    {
        openHistoryAction_->setEnabled(enabled);
    }
    if (historyButton_)
    {
        historyButton_->setEnabled(enabled);
    }
}

void MainWindow::clearEventTable()
{
    if (uiEventDrainTimer_)
    {
        uiEventDrainTimer_->stop();
    }
    pendingUiEvents_.clear();
    eventModel_->clear();
}

void MainWindow::onEventFilterChanged(const QString& filter)
{
    ++historyQueryGeneration_;
    ++liveCountRequestId_;
    eventCountQueryFailed_ = false;
    liveCountDeltaActive_ = false;
    if (historyFilterThread_)
    {
        historyFilterThread_->requestInterruption();
    }
    if (liveCountQueryThread_)
    {
        liveCountQueryThread_->requestInterruption();
        liveCountRefreshPending_ = true;
    }
    if (liveCountSnapshotPending_)
    {
        liveCountRefreshPending_ = true;
    }

    if (historyDatabasePath_.isEmpty())
    {
        eventProxy_->setFilterFixedString(filter);
        if (filter.isEmpty())
        {
            eventFilteredCount_ = eventTotalCount_;
            messageFilteredCount_ = messageTotalCount_;
            eventFilteredCountKnown_ = true;
            liveCountRefreshPending_ = false;
            eventFilterTimer_->stop();
        }
        else if (eventRunSerial_ == 0)
        {
            eventFilteredCount_ = eventProxy_->rowCount();
            messageFilteredCount_ = 0;
            eventFilteredCountKnown_ = true;
            liveCountRefreshPending_ = false;
            eventFilterTimer_->stop();
        }
        else
        {
            eventFilteredCountKnown_ = false;
            eventFilterTimer_->start();
        }
    }
    else
    {
        // The history query already applies the filter to the complete
        // database; applying the proxy again would only re-filter the window.
        eventProxy_->setFilterFixedString(QString{});
        if (filter.isEmpty())
        {
            eventFilteredCount_ = eventTotalCount_;
            messageFilteredCount_ = messageTotalCount_;
            eventFilteredCountKnown_ = true;
        }
        else
        {
            eventFilteredCountKnown_ = false;
        }
        eventFilterTimer_->start();
    }
    updateEventCountLabel();
}

void MainWindow::refreshEventFilter()
{
    if (closing_ || simulationStartPending_)
    {
        return;
    }
    const auto filter = eventFilterEdit_->text();
    if (!historyDatabasePath_.isEmpty())
    {
        if (!historyFilterQueryInProgress_)
        {
            startHistoryFilterQuery(historyQueryGeneration_, historyDatabasePath_, filter);
        }
        return;
    }
    if (filter.isEmpty() || eventRunSerial_ == 0)
    {
        return;
    }

    if (liveCountQueryThread_)
    {
        liveCountQueryThread_->requestInterruption();
        liveCountRefreshPending_ = true;
        return;
    }
    if (liveCountSnapshotPending_)
    {
        liveCountRefreshPending_ = true;
        return;
    }

    liveCountSnapshotPending_ = true;
    liveCountRefreshPending_ = false;
    const auto requestId = liveCountRequestId_;
    QMetaObject::invokeMethod(eventStore_,
                              [store = eventStore_, requestId] { store->requestCountSnapshot(requestId); },
                              Qt::QueuedConnection);
}

void MainWindow::startHistoryFilterQuery(quint64 generation, const QString& path, const QString& filter)
{
    historyFilterQueryInProgress_ = true;
    auto state = std::make_shared<HistoryLoadState>();
    auto* loadThread = QThread::create(
        [state, path, filter]
        {
            const auto cancelled = [] { return QThread::currentThread()->isInterruptionRequested(); };
            state->page = EventStore::queryDatabase(path, static_cast<int>(EventTableModel::liveCapacity()), filter, &state->error, {},
                                                    cancelled);
        });
    historyFilterThread_ = loadThread;
    trackQueryThread(loadThread);
    connect(loadThread, &QThread::finished, this,
            [this, state, generation, path, loadThread]
            {
                loadThread->deleteLater();
                if (historyFilterThread_ == loadThread)
                {
                    historyFilterThread_ = nullptr;
                }
                historyFilterQueryInProgress_ = false;
                const auto current = generation == historyQueryGeneration_ && path == historyDatabasePath_ && !closing_ &&
                                     !simulationRunning_ && !simulationStartPending_;
                if (current)
                {
                    if (!state->error.isEmpty())
                    {
                        statusBar()->showMessage(state->error, 5000);
                        eventFilteredCountKnown_ = false;
                        eventCountQueryFailed_ = true;
                        updateEventCountLabel();
                    }
                    else
                    {
                        eventTotalCount_ = state->page.totalCount;
                        eventFilteredCount_ = state->page.filteredCount;
                        messageTotalCount_ = state->page.messageTotalCount;
                        messageFilteredCount_ = state->page.filteredMessageCount;
                        eventFilteredCountKnown_ = true;
                        eventCountQueryFailed_ = false;
                        eventModel_->setEvents(std::move(state->page.events));
                        updateEventCountLabel();
                    }
                }
                else if (!historyDatabasePath_.isEmpty() && !historyLoadInProgress_ && !closing_ && !simulationRunning_ &&
                         !simulationStartPending_)
                {
                    eventFilterTimer_->start();
                }
            });
    loadThread->start();
}

void MainWindow::startLiveCountQuery(quint64 requestId, quint64 runSerial, const QString& path, quint64 maxEventId,
                                     const QString& filter)
{
    auto state = std::make_shared<LiveCountQueryState>();
    auto* queryThread = QThread::create(
        [state, path, filter, maxEventId]
        {
            const auto cancelled = [] { return QThread::currentThread()->isInterruptionRequested(); };
            state->page = EventStore::countDatabase(path, filter, maxEventId, &state->error, cancelled);
        });
    liveCountQueryThread_ = queryThread;
    trackQueryThread(queryThread);
    connect(queryThread, &QThread::finished, this,
            [this, state, requestId, runSerial, queryThread]
            {
                queryThread->deleteLater();
                if (liveCountQueryThread_ == queryThread)
                {
                    liveCountQueryThread_ = nullptr;
                }
                const auto current = requestId == liveCountRequestId_ && runSerial == eventRunSerial_ &&
                                     historyDatabasePath_.isEmpty() && !closing_ && !simulationStartPending_;
                if (current && state->error.isEmpty())
                {
                    eventTotalCount_ = state->page.totalCount + liveDeltaEventTotal_;
                    eventFilteredCount_ = state->page.filteredCount + liveDeltaEventFiltered_;
                    messageTotalCount_ = state->page.messageTotalCount + liveDeltaMessageTotal_;
                    messageFilteredCount_ = state->page.filteredMessageCount + liveDeltaMessageFiltered_;
                    eventFilteredCountKnown_ = true;
                    eventCountQueryFailed_ = false;
                    liveCountDeltaActive_ = false;
                    updateEventCountLabel();
                }
                else if (current)
                {
                    eventFilteredCountKnown_ = false;
                    eventCountQueryFailed_ = true;
                    liveCountDeltaActive_ = false;
                    statusBar()->showMessage(state->error, 5000);
                    updateEventCountLabel();
                }
                else
                {
                    liveCountDeltaActive_ = false;
                }

                if (liveCountRefreshPending_ && !closing_ && historyDatabasePath_.isEmpty() && !eventFilterTimer_->isActive())
                {
                    liveCountRefreshPending_ = false;
                    QTimer::singleShot(0, this, &MainWindow::refreshEventFilter);
                }
            });
    queryThread->start();
}

void MainWindow::trackQueryThread(QThread* thread)
{
    thread->setParent(this);
    queryThreads_.insert(thread);
    connect(thread, &QThread::finished, this,
            [this, thread]
            {
                queryThreads_.remove(thread);
            });
}

void MainWindow::stopAndWaitForQueryThreads()
{
    const auto threads = queryThreads_.values();
    for (auto* thread : threads)
    {
        thread->requestInterruption();
    }
    for (auto* thread : threads)
    {
        thread->wait();
    }
    queryThreads_.clear();
    historyLoadThread_ = nullptr;
    historyFilterThread_ = nullptr;
    liveCountQueryThread_ = nullptr;
    historyFilterQueryInProgress_ = false;
    liveCountSnapshotPending_ = false;
    liveCountDeltaActive_ = false;
}

void MainWindow::connectEngine()
{
    engine_ = new SimulationEngine;
    engine_->moveToThread(&engineThread_);
    connect(&engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
    connect(engine_, &SimulationEngine::runningChanged, this, &MainWindow::onRunningChanged);
    connect(engine_, &SimulationEngine::convergenceChanged, this, &MainWindow::onConvergenceChanged);
    connect(engine_, &SimulationEngine::statsChanged, this, &MainWindow::onStatsChanged);
    connect(engine_, &SimulationEngine::startupProgress, this, &MainWindow::onStartupProgress);
    connect(engine_, &SimulationEngine::startupCancelled, this, &MainWindow::onStartupCancelled);
    connect(engine_, &SimulationEngine::eventsGenerated, eventStore_, &EventStore::enqueueEvents, Qt::DirectConnection);
    connect(engine_, &SimulationEngine::routerSnapshotsChanged, this, &MainWindow::onRouterSnapshots);
    connect(engine_, &SimulationEngine::ribSnapshotReady, this, &MainWindow::onRibSnapshot);
    connect(engine_, &SimulationEngine::peersSnapshotReady, this, &MainWindow::onPeerSnapshots);
    connect(engine_, &SimulationEngine::routingStateChanged, this, &MainWindow::onRoutingStateChanged);
    connect(engine_, &SimulationEngine::pathReady, this, &MainWindow::onPathReady);
    connect(engine_, &SimulationEngine::routerStateChanged, this, &MainWindow::onRouterRuntimeState);
    connect(engine_, &SimulationEngine::linkStateChanged, this, &MainWindow::onLinkRuntimeState);
    connect(engine_, &SimulationEngine::errorOccurred, this, &MainWindow::onEngineError);
    engineThread_.setObjectName(QStringLiteral("BgpSimulationThread"));
    engineThread_.start();
}

void MainWindow::newTopology()
{
    if (!maybeSave())
    {
        return;
    }
    setTopology(starterTopology());
    setDirty(false);
}

void MainWindow::openTopology()
{
    if (!maybeSave())
    {
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 BGP 拓扑"),
                                                   topologyPath_.isEmpty() ? QDir::currentPath() : QFileInfo(topologyPath_).absolutePath(),
                                                   QStringLiteral("Topology JSON (*.json);;所有文件 (*.*)"));
    if (!path.isEmpty())
    {
        openTopologyPath(path);
    }
}

bool MainWindow::openTopologyPath(const QString& path, bool showErrors)
{
    if (closing_ || topologyLoadInProgress_)
    {
        return false;
    }

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable())
    {
        if (showErrors)
        {
            QMessageBox::critical(this, QStringLiteral("无法打开拓扑"),
                                  QStringLiteral("无法读取 %1").arg(fileInfo.absoluteFilePath()));
        }
        return false;
    }

    const auto absolutePath = fileInfo.absoluteFilePath();
    const auto generation = ++topologyLoadGeneration_;
    auto state = std::make_shared<TopologyLoadState>();
    state->totalBytes.store(fileInfo.size(), std::memory_order_relaxed);

    auto* progressDialog = new QProgressDialog(QStringLiteral("正在读取拓扑…"), QStringLiteral("取消"), 0, 1000, this);
    progressDialog->setWindowTitle(QStringLiteral("加载拓扑"));
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setValue(0);

    auto* progressTimer = new QTimer(progressDialog);
    progressTimer->setInterval(100);
    connect(progressTimer, &QTimer::timeout, progressDialog,
            [state, progressDialog]
            {
                const auto processed = state->bytesProcessed.load(std::memory_order_relaxed);
                const auto total = state->totalBytes.load(std::memory_order_relaxed);
                const auto routers = state->routersLoaded.load(std::memory_order_relaxed);
                const auto links = state->linksLoaded.load(std::memory_order_relaxed);
                const auto stage = static_cast<TopologyLoadStage>(state->stage.load(std::memory_order_relaxed));
                const auto stageText = stage == TopologyLoadStage::ReadingRouters
                                           ? QStringLiteral("正在解析路由器")
                                           : (stage == TopologyLoadStage::ReadingLinks ? QStringLiteral("正在解析链路")
                                                                                       : QStringLiteral("正在校验拓扑"));
                if (total > 0)
                {
                    const auto value = stage == TopologyLoadStage::Validating
                                           ? 1000
                                           : static_cast<int>(std::clamp<qint64>(processed * 1000 / total, 0, 999));
                    progressDialog->setValue(value);
                    progressDialog->setLabelText(
                        QStringLiteral("%1… %2 / %3 MiB · %4 台路由器 · %5 条链路")
                            .arg(stageText)
                            .arg(processed / (1024.0 * 1024.0), 0, 'f', 1)
                            .arg(total / (1024.0 * 1024.0), 0, 'f', 1)
                            .arg(routers)
                            .arg(links));
                }
                else
                {
                    progressDialog->setLabelText(QStringLiteral("%1… %2 台路由器 · %3 条链路").arg(stageText).arg(routers).arg(links));
                }
            });

    auto* loadThread = QThread::create(
        [state, absolutePath]
        {
            state->topology = Topology::load(
                absolutePath, &state->error,
                [state](const TopologyLoadProgress& progress)
                {
                    state->bytesProcessed.store(progress.bytesProcessed, std::memory_order_relaxed);
                    state->totalBytes.store(progress.totalBytes, std::memory_order_relaxed);
                    state->routersLoaded.store(progress.routersLoaded, std::memory_order_relaxed);
                    state->linksLoaded.store(progress.linksLoaded, std::memory_order_relaxed);
                    state->stage.store(static_cast<int>(progress.stage), std::memory_order_relaxed);
                    return !state->cancelRequested.load(std::memory_order_relaxed) &&
                           !QThread::currentThread()->isInterruptionRequested();
                });
        });
    loadThread->setObjectName(QStringLiteral("TopologyLoaderThread"));
    loadThread->setParent(this);
    topologyLoadThread_ = loadThread;
    topologyLoadInProgress_ = true;
    updateEditorActions();

    connect(progressDialog, &QProgressDialog::canceled, loadThread,
            [state, loadThread]
            {
                state->cancelRequested.store(true, std::memory_order_relaxed);
                loadThread->requestInterruption();
            });
    connect(loadThread, &QThread::finished, this,
            [this, state, absolutePath, generation, showErrors, progressDialog, progressTimer, loadThread]
            {
                const auto interrupted = loadThread->isInterruptionRequested();
                QObject::disconnect(progressDialog, nullptr, loadThread, nullptr);
                progressTimer->stop();
                progressDialog->close();
                progressDialog->deleteLater();
                loadThread->deleteLater();
                if (topologyLoadThread_ == loadThread)
                {
                    topologyLoadThread_ = nullptr;
                }
                topologyLoadInProgress_ = false;
                if (closing_ || generation != topologyLoadGeneration_)
                {
                    return;
                }
                updateEditorActions();
                if (state->cancelRequested.load(std::memory_order_relaxed) || interrupted)
                {
                    statusBar()->showMessage(QStringLiteral("已取消加载拓扑"), 4000);
                    return;
                }
                if (!state->topology)
                {
                    if (showErrors)
                    {
                        QMessageBox::critical(this, QStringLiteral("无法打开拓扑"), state->error);
                    }
                    else
                    {
                        statusBar()->showMessage(QStringLiteral("自动加载拓扑失败：%1").arg(state->error), 8000);
                    }
                    return;
                }

                const auto routerCount = state->topology->routers.size();
                const auto linkCount = state->topology->links.size();
                setTopology(std::move(*state->topology), absolutePath);
                setDirty(false);
                QSettings().setValue(QStringLiteral("files/lastTopology"), topologyPath_);
                statusBar()->showMessage(
                    QStringLiteral("已加载 %1（%2 台路由器，%3 条链路%4）")
                        .arg(topologyPath_)
                        .arg(routerCount)
                        .arg(linkCount)
                        .arg(scene_->usesOverviewRendering() ? QStringLiteral("，概览渲染") : QString{}),
                    8000);
            });

    progressTimer->start();
    progressDialog->show();
    loadThread->start();
    return true;
}

void MainWindow::stopTopologyLoad()
{
    ++topologyLoadGeneration_;
    auto* thread = topologyLoadThread_;
    topologyLoadThread_ = nullptr;
    topologyLoadInProgress_ = false;
    if (!thread)
    {
        return;
    }
    thread->requestInterruption();
    if (thread->isRunning())
    {
        thread->wait();
    }
}

bool MainWindow::saveTopology()
{
    if (topologyPath_.isEmpty())
    {
        return saveTopologyAs();
    }
    QString error;
    if (!topology_.save(topologyPath_, &error))
    {
        QMessageBox::critical(this, QStringLiteral("保存失败"), error);
        return false;
    }
    setDirty(false);
    QSettings().setValue(QStringLiteral("files/lastTopology"), topologyPath_);
    statusBar()->showMessage(QStringLiteral("已保存 %1").arg(topologyPath_), 4000);
    return true;
}

bool MainWindow::saveTopologyAs()
{
    auto suggested = topologyPath_;
    if (suggested.isEmpty())
    {
        suggested = QDir::current().filePath(QStringLiteral("%1.json").arg(topology_.simulation.name));
    }
    auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存 BGP 拓扑"), suggested, QStringLiteral("Topology JSON (*.json)"));
    if (path.isEmpty())
    {
        return false;
    }
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
    {
        path += QStringLiteral(".json");
    }
    topologyPath_ = QFileInfo(path).absoluteFilePath();
    return saveTopology();
}

void MainWindow::openHistory()
{
    if (closing_ || simulationRunning_ || simulationStartPending_ || historyLoadInProgress_ || topologyLoadInProgress_)
    {
        return;
    }

    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 BMP SQLite 日志"), QDir::currentPath(),
                                                   QStringLiteral("SQLite database (*.sqlite *.db);;所有文件 (*.*)"));
    if (path.isEmpty())
    {
        return;
    }

    // A run that has just stopped may still have a queued final transaction.
    // Commit it before a read-only history connection takes its snapshot.
    if (eventStore_ && eventStoreThread_.isRunning())
    {
        QMetaObject::invokeMethod(eventStore_, &EventStore::flush, Qt::BlockingQueuedConnection);
    }

    historyLoadInProgress_ = true;
    updateEditorActions();
    const auto generation = ++historyQueryGeneration_;
    if (historyFilterThread_)
    {
        historyFilterThread_->requestInterruption();
    }
    const auto filter = eventFilterEdit_->text();
    auto state = std::make_shared<HistoryLoadState>();

    auto* progressDialog = new QProgressDialog(this);
    progressDialog->setWindowTitle(QStringLiteral("加载 SQLite 日志"));
    progressDialog->setLabelText(QStringLiteral("正在读取历史记录…"));
    progressDialog->setCancelButtonText(QStringLiteral("取消加载"));
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setRange(0, 0);
    connect(progressDialog, &QProgressDialog::canceled, progressDialog,
            [state] { state->cancelRequested.store(true, std::memory_order_relaxed); });
    progressDialog->show();

    auto* progressTimer = new QTimer(progressDialog);
    progressTimer->setInterval(100);
    connect(progressTimer, &QTimer::timeout, progressDialog,
            [state, progressDialog]
            {
                const auto loaded = state->loadedRows.load(std::memory_order_relaxed);
                const auto total = state->totalRows.load(std::memory_order_relaxed);
                if (total <= 0)
                {
                    progressDialog->setRange(0, 0);
                    progressDialog->setLabelText(QStringLiteral("正在读取历史记录… 已加载 %1 条").arg(loaded));
                    return;
                }

                progressDialog->setRange(0, 1000);
                progressDialog->setValue(static_cast<int>(std::min<qint64>(1000, loaded * 1000 / total)));
                progressDialog->setLabelText(QStringLiteral("正在读取历史记录… %1 / %2 条").arg(loaded).arg(total));
            });
    progressTimer->start();

    auto* loadThread = QThread::create(
        [state, path, filter]
        {
            const auto cancelled = [state]
            {
                return state->cancelRequested.load(std::memory_order_relaxed) ||
                       QThread::currentThread()->isInterruptionRequested();
            };
            state->page = EventStore::queryDatabase(path, static_cast<int>(EventTableModel::liveCapacity()), filter, &state->error,
                                                    [state](qsizetype loaded, qsizetype total)
                                                    {
                                                        state->loadedRows.store(loaded, std::memory_order_relaxed);
                                                        state->totalRows.store(total, std::memory_order_relaxed);
                                                        return !state->cancelRequested.load(std::memory_order_relaxed) &&
                                                               !QThread::currentThread()->isInterruptionRequested();
                                                    },
                                                    cancelled);
            if (state->error.isEmpty() && !cancelled())
            {
                state->convergencePage =
                    EventStore::queryConvergenceDatabase(path, convergenceHistoryCapacity, &state->convergenceError, cancelled);
            }
        });
    historyLoadThread_ = loadThread;
    trackQueryThread(loadThread);
    connect(progressDialog, &QProgressDialog::canceled, loadThread, &QThread::requestInterruption);
    connect(loadThread, &QThread::finished, this,
            [this, state, path, generation, progressDialog, loadThread]
            {
                loadThread->deleteLater();
                if (historyLoadThread_ == loadThread)
                {
                    historyLoadThread_ = nullptr;
                }
                historyLoadInProgress_ = false;
                updateEditorActions();
                progressDialog->setRange(0, 1000);
                progressDialog->setValue(1000);

                if (generation != historyQueryGeneration_ || closing_ || simulationRunning_ || simulationStartPending_)
                {
                    progressDialog->deleteLater();
                    if (!closing_ && !simulationRunning_ && !simulationStartPending_ && !eventFilteredCountKnown_)
                    {
                        eventFilterTimer_->start();
                    }
                    return;
                }

                if (state->cancelRequested.load(std::memory_order_relaxed))
                {
                    progressDialog->deleteLater();
                    statusBar()->showMessage(QStringLiteral("已取消加载历史 BMP 日志"), 4000);
                    if (!eventFilteredCountKnown_)
                    {
                        eventFilterTimer_->start();
                    }
                    return;
                }

                if (!state->error.isEmpty())
                {
                    progressDialog->deleteLater();
                    QMessageBox::critical(this, QStringLiteral("无法读取日志"), state->error);
                    if (!eventFilteredCountKnown_)
                    {
                        eventFilterTimer_->start();
                    }
                    return;
                }

                const auto eventCount = state->page.events.size();
                progressDialog->setLabelText(QStringLiteral("正在更新日志表格…"));
                historyDatabasePath_ = path;
                ++liveCountRequestId_;
                if (liveCountQueryThread_)
                {
                    liveCountQueryThread_->requestInterruption();
                }
                liveCountSnapshotPending_ = false;
                liveCountRefreshPending_ = false;
                eventTotalCount_ = state->page.totalCount;
                eventFilteredCount_ = state->page.filteredCount;
                messageTotalCount_ = state->page.messageTotalCount;
                messageFilteredCount_ = state->page.filteredMessageCount;
                eventFilteredCountKnown_ = true;
                eventCountQueryFailed_ = false;
                liveCountDeltaActive_ = false;
                eventProxy_->setFilterFixedString(QString{});
                eventProxy_->sort(-1);
                uiEventDrainTimer_->stop();
                pendingUiEvents_.clear();
                if (state->convergenceError.isEmpty())
                {
                    rebuildConvergenceHistory(state->convergencePage.events, state->convergencePage.totalCount);
                }
                else
                {
                    clearConvergenceHistory(QStringLiteral("历史收敛记录读取失败：%1").arg(state->convergenceError));
                }
                eventModel_->setEvents(std::move(state->page.events));
                followEventsCheck_->setChecked(false);
                logPathLabel_->setText(QStringLiteral("历史：%1").arg(QDir::toNativeSeparators(path)));
                logPathLabel_->setToolTip(path);
                updateEventCountLabel();
                statusBar()->showMessage(QStringLiteral("已显示最近 %1 条事件；事件 %2/%3，报文 %4/%5")
                                             .arg(eventCount)
                                             .arg(eventFilteredCount_)
                                             .arg(eventTotalCount_)
                                             .arg(messageFilteredCount_)
                                             .arg(messageTotalCount_),
                                         5000);
                progressDialog->deleteLater();
            });
    loadThread->start();
}

void MainWindow::editSimulationSettings()
{
    SimulationSettingsDialog dialog(topology_.simulation, this);
    if (dialog.exec() == QDialog::Accepted)
    {
        topology_.simulation = dialog.settings();
        setDirty(true);
    }
}

void MainWindow::editTopologyBatchProperties()
{
    const auto selectedRouterIds = scene_->selectedRouterIds();
    const auto selectedRoutersOnly = selectedRouterIds.size() > 1;
    const auto targetRouterIds = selectedRoutersOnly ? selectedRouterIds : topology_.routers.keys();

    QVector<RouterConfig> targetRouters;
    targetRouters.reserve(targetRouterIds.size());
    for (const auto& routerId : targetRouterIds)
    {
        const auto router = topology_.routers.constFind(routerId);
        if (router != topology_.routers.cend())
        {
            targetRouters.append(router.value());
        }
    }

    if (topology_.links.isEmpty() && targetRouters.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("没有可配置对象"), QStringLiteral("当前拓扑没有可批量配置的路由器或链路。"));
        return;
    }

    const auto routerScope =
        selectedRoutersOnly ? TopologyBatchEditDialog::RouterScope::Selection : TopologyBatchEditDialog::RouterScope::All;
    TopologyBatchEditDialog dialog(topology_.links, targetRouters, routerScope, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    auto changedRouters = 0;
    auto changedLinks = 0;
    auto changedMraiDirections = 0;
    QStringList appliedOperations;
    const auto targetDescription = selectedRoutersOnly ? QStringLiteral("选中的 %1 台路由器").arg(targetRouters.size())
                                                       : QStringLiteral("全部 %1 台路由器").arg(targetRouters.size());

    const auto pluginId = dialog.routerPluginId();
    if (!pluginId.isEmpty())
    {
        const auto defaultSettings = dialog.routerPluginDefaultSettings();
        for (const auto& routerId : targetRouterIds)
        {
            auto router = topology_.routers.find(routerId);
            if (router != topology_.routers.end() && router->pluginId != pluginId)
            {
                router->pluginId = pluginId;
                router->pluginSettings = defaultSettings;
                ++changedRouters;
            }
        }
        appliedOperations.append(QStringLiteral("已将%1的种类设置为 %2").arg(targetDescription, pluginId));
    }

    QMap<QString, int> mraiByRouter;
    if (dialog.mraiMode() == TopologyBatchEditDialog::MraiMode::Fixed)
    {
        const auto mraiMs = dialog.fixedMraiMs();
        for (const auto& routerId : targetRouterIds)
        {
            mraiByRouter.insert(routerId, mraiMs);
        }
        appliedOperations.append(QStringLiteral("已将%1的出站 MRAI 设置为 %2 ms").arg(targetDescription).arg(mraiMs));
    }
    else if (dialog.mraiMode() == TopologyBatchEditDialog::MraiMode::RandomRange)
    {
        const auto minimumMraiMs = dialog.minimumMraiMs();
        const auto maximumMraiMs = dialog.maximumMraiMs();
        const auto valueCount = static_cast<quint32>(maximumMraiMs - minimumMraiMs + 1);
        for (const auto& routerId : targetRouterIds)
        {
            mraiByRouter.insert(routerId, minimumMraiMs + static_cast<int>(QRandomGenerator::global()->bounded(valueCount)));
        }
        appliedOperations.append(QStringLiteral("已将%1的出站 MRAI 按路由器随机设置在 %2–%3 ms 范围内")
                                     .arg(targetDescription)
                                     .arg(minimumMraiMs)
                                     .arg(maximumMraiMs));
    }
    if (!mraiByRouter.isEmpty())
    {
        for (auto& link : topology_.links)
        {
            const auto fromA = mraiByRouter.constFind(link.a);
            if (fromA != mraiByRouter.cend() && link.mraiMsFromA != fromA.value())
            {
                link.mraiMsFromA = fromA.value();
                ++changedMraiDirections;
            }
            const auto fromB = mraiByRouter.constFind(link.b);
            if (fromB != mraiByRouter.cend() && link.mraiMsFromB != fromB.value())
            {
                link.mraiMsFromB = fromB.value();
                ++changedMraiDirections;
            }
        }
    }

    if (dialog.delayMode() == TopologyBatchEditDialog::DelayMode::Fixed)
    {
        const auto delayMs = dialog.fixedDelayMs();
        for (auto& link : topology_.links)
        {
            if (link.delayMs != delayMs)
            {
                link.delayMs = delayMs;
                ++changedLinks;
            }
        }
        appliedOperations.append(QStringLiteral("已将全部 %1 条链路的延迟设置为 %2 ms").arg(topology_.links.size()).arg(delayMs));
    }
    else if (dialog.delayMode() == TopologyBatchEditDialog::DelayMode::RandomRange)
    {
        const auto minimumDelayMs = dialog.minimumDelayMs();
        const auto maximumDelayMs = dialog.maximumDelayMs();
        const auto valueCount = static_cast<quint32>(maximumDelayMs - minimumDelayMs + 1);
        for (auto& link : topology_.links)
        {
            const auto delayMs = minimumDelayMs + static_cast<int>(QRandomGenerator::global()->bounded(valueCount));
            if (link.delayMs != delayMs)
            {
                link.delayMs = delayMs;
                ++changedLinks;
            }
        }
        appliedOperations.append(QStringLiteral("已将全部 %1 条链路的延迟随机设置在 %2–%3 ms 范围内")
                                     .arg(topology_.links.size())
                                     .arg(minimumDelayMs)
                                     .arg(maximumDelayMs));
    }

    statusBar()->showMessage(
        appliedOperations.isEmpty() ? QStringLiteral("未选择要修改的批量配置项") : appliedOperations.join(QStringLiteral("；")), 5000);

    if (changedRouters > 0 || changedLinks > 0 || changedMraiDirections > 0)
    {
        scene_->rebuild();
        refreshTopologySelectors();
        setDirty(true);
    }
}

void MainWindow::createRouter(const QPointF& position)
{
    RouterConfig config;
    config.id = topology_.nextRouterName();
    config.routerId = topology_.nextBgpRouterId();
    config.clusterId = config.routerId;
    config.position = position;
    RouterDialog dialog(config, topology_.routers.keys(), this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto router = dialog.router();
    topology_.routers.insert(router.id, router);
    scene_->rebuild();
    refreshTopologySelectors();
    setDirty(true);
}

void MainWindow::editRouter(const QString& routerId)
{
    const auto it = topology_.routers.constFind(routerId);
    if (it == topology_.routers.cend())
    {
        return;
    }
    auto otherIds = topology_.routers.keys();
    otherIds.removeAll(routerId);
    RouterDialog dialog(it.value(), otherIds, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto updated = dialog.router();
    if (updated.id != routerId)
    {
        topology_.routers.remove(routerId);
        for (auto& link : topology_.links)
        {
            if (link.a == routerId)
            {
                link.a = updated.id;
            }
            if (link.b == routerId)
            {
                link.b = updated.id;
            }
        }
    }
    topology_.routers.insert(updated.id, updated);
    for (auto& link : topology_.links)
    {
        if (link.businessRelationship == LinkBusinessRelationship::Unspecified ||
            (link.a != updated.id && link.b != updated.id))
        {
            continue;
        }
        const auto a = topology_.routers.constFind(link.a);
        const auto b = topology_.routers.constFind(link.b);
        if (a != topology_.routers.cend() && b != topology_.routers.cend() && a->asn == b->asn)
        {
            link.businessRelationship = LinkBusinessRelationship::Unspecified;
        }
    }
    scene_->rebuild();
    refreshTopologySelectors();
    routerCombo_->setCurrentText(updated.id);
    setDirty(true);
}

void MainWindow::createLink(const QString& a, const QString& b)
{
    if (topology_.findLink(a, b))
    {
        QMessageBox::information(this, QStringLiteral("链路已存在"), QStringLiteral("%1 与 %2 已经连接。").arg(a, b));
        return;
    }
    LinkConfig config{.a = a, .b = b};
    LinkDialog dialog(config, topology_.routers.value(a).asn != topology_.routers.value(b).asn, this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    topology_.links.append(dialog.link());
    scene_->rebuild();
    refreshTopologySelectors();
    setDirty(true);
}

void MainWindow::editLink(const QString& a, const QString& b)
{
    auto* link = topology_.findLink(a, b);
    if (!link)
    {
        return;
    }
    LinkDialog dialog(*link, topology_.routers.value(a).asn != topology_.routers.value(b).asn, this);
    if (dialog.exec() == QDialog::Accepted)
    {
        *link = dialog.link();
        scene_->rebuild();
        refreshTopologySelectors();
        setDirty(true);
    }
}

void MainWindow::deleteSelection()
{
    scene_->deleteSelection();
}

void MainWindow::markDirty()
{
    refreshTopologySelectors();
    setDirty(true);
}

void MainWindow::startSimulation()
{
    if (closing_ || simulationRunning_ || simulationStartPending_ || historyLoadInProgress_)
    {
        return;
    }
    ++historyQueryGeneration_;
    ++liveCountRequestId_;
    if (historyLoadThread_)
    {
        historyLoadThread_->requestInterruption();
    }
    if (historyFilterThread_)
    {
        historyFilterThread_->requestInterruption();
    }
    if (liveCountQueryThread_)
    {
        liveCountQueryThread_->requestInterruption();
    }
    historyDatabasePath_.clear();
    eventFilterTimer_->stop();
    eventTotalCount_ = 0;
    eventFilteredCount_ = 0;
    messageTotalCount_ = 0;
    messageFilteredCount_ = 0;
    eventFilteredCountKnown_ = true;
    eventCountQueryFailed_ = false;
    liveCountSnapshotPending_ = false;
    liveCountRefreshPending_ = false;
    liveCountDeltaActive_ = false;
    eventProxy_->setFilterFixedString(eventFilterEdit_->text());
    clearEventTable();
    clearConvergenceHistory(QStringLiteral("当前：正在创建 BMP 日志"));
    followEventsCheck_->setChecked(true);
    runtimeRouters_.clear();
    runtimeLinks_.clear();
    scene_->clearRuntimeState();
    scene_->setEditable(false);
    simulationStartPending_ = true;
    simulationStartCancelRequested_ = false;
    engine_->prepareStartup();
    simulationStatusLabel_->setText(QStringLiteral("● 正在启动"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#d97706"));
    updateEditorActions();
    const auto settings = topology_.simulation;
    const auto topology = topology_;
    const QPointer<MainWindow> self(this);
    auto* store = eventStore_;
    const auto invoked = QMetaObject::invokeMethod(
        store,
        [self, store, settings, topology]() mutable
        {
            QString error;
            const auto started = store->beginRun(settings, &error);
            const auto runSerial = store->runSerial();
            if (!self)
            {
                if (started)
                {
                    store->endRun();
                }
                return;
            }
            const auto callbackQueued = QMetaObject::invokeMethod(
                self.data(),
                [self, started, runSerial, error = std::move(error), topology = std::move(topology)]() mutable
                {
                    if (!self)
                    {
                        return;
                    }
                    if (self->closing_ || self->simulationStartCancelRequested_)
                    {
                        self->simulationStartPending_ = false;
                        self->simulationStartCancelRequested_ = false;
                        if (started)
                        {
                            self->endEventRun(false);
                        }
                        self->simulationStatusLabel_->setText(QStringLiteral("● 已停止"));
                        self->simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#6c757d"));
                        self->convergenceStateLabel_->setText(QStringLiteral("当前：已取消启动"));
                        self->scene_->setEditable(!self->closing_);
                        self->updateEditorActions();
                        return;
                    }
                    if (!started)
                    {
                        self->simulationStartPending_ = false;
                        self->simulationStatusLabel_->setText(QStringLiteral("● 启动失败"));
                        self->simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#b02a37"));
                        self->convergenceStateLabel_->setText(QStringLiteral("当前：启动失败"));
                        self->scene_->setEditable(true);
                        self->updateEditorActions();
                        QMessageBox::critical(self, QStringLiteral("无法创建日志"), error);
                        return;
                    }
                    self->eventRunSerial_ = runSerial;
                    QMetaObject::invokeMethod(self->engine_,
                                              [engine = self->engine_, topology = std::move(topology)]() mutable
                                              { engine->startSimulation(std::move(topology)); },
                                              Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
            if (!callbackQueued && started)
            {
                store->endRun();
            }
        },
        Qt::QueuedConnection);
    if (!invoked)
    {
        simulationStartPending_ = false;
        simulationStatusLabel_->setText(QStringLiteral("● 启动失败"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#b02a37"));
        convergenceStateLabel_->setText(QStringLiteral("当前：启动失败"));
        scene_->setEditable(true);
        updateEditorActions();
        QMessageBox::critical(this, QStringLiteral("无法创建日志"), QStringLiteral("无法调用 BMP 事件持久化线程"));
    }
}

void MainWindow::stopSimulation()
{
    stopAction_->setEnabled(false);
    if (simulationStartPending_ && !simulationRunning_)
    {
        simulationStartCancelRequested_ = true;
        engine_->requestStartupCancellation();
        QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::QueuedConnection);
        simulationStatusLabel_->setText(QStringLiteral("● 正在取消启动"));
        convergenceStateLabel_->setText(QStringLiteral("当前：正在取消启动"));
        return;
    }
    QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::QueuedConnection);
}

void MainWindow::onRunningChanged(bool running)
{
    simulationStartPending_ = false;
    simulationStartCancelRequested_ = false;
    simulationRunning_ = running;
    scene_->setEditable(!running);
    updateEditorActions();
    if (!running)
    {
        flushEventStore();
        snapshotRequestPending_ = false;
        snapshotRefreshNeeded_ = false;
        simulationStatusLabel_->setText(QStringLiteral("● 已停止"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#6c757d"));
        convergenceStateLabel_->setText(QStringLiteral("当前：已停止"));
    }
    else
    {
        simulationStatusLabel_->setText(QStringLiteral("● 收敛中"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#d97706"));
        convergenceStateLabel_->setText(QStringLiteral("当前：收敛中 · 已用时 0 ms"));
        scheduleSelectedRouterSnapshot();
    }
}

void MainWindow::onConvergenceChanged(bool converged)
{
    simulationConverged_ = converged;
    if (simulationRunning_ && converged)
    {
        simulationStatusLabel_->setText(QStringLiteral("● 已收敛"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#16835d"));
        convergenceStateLabel_->setText(QStringLiteral("当前：已收敛"));
    }
    else if (simulationRunning_)
    {
        simulationStatusLabel_->setText(QStringLiteral("● 收敛中"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#d97706"));
        convergenceStateLabel_->setText(QStringLiteral("当前：收敛中 · 已用时 0 ms"));
    }
}

void MainWindow::onStatsChanged(const SimulationStats& stats)
{
    statsLabel_->setText(
        QStringLiteral("消息 %1 · 待处理 %2 · %3 ms").arg(stats.deliveredMessages).arg(stats.pendingEvents).arg(stats.elapsedMs));
    if (stats.running && !stats.converged)
    {
        convergenceStateLabel_->setText(QStringLiteral("当前：收敛中 · 触发：%1 · 已用时 %2")
                                            .arg(convergenceTriggerText(stats.convergenceTriggerEvent, stats.convergenceTriggerContext),
                                                 durationText(stats.convergenceElapsedMs)));
    }
}

void MainWindow::onRouterSnapshots(QVector<RouterSnapshot> snapshots)
{
    runtimeRouters_.clear();
    QMap<QString, bool> runtimeStates;
    for (const auto& snapshot : snapshots)
    {
        runtimeRouters_.insert(snapshot.id, snapshot);
        runtimeStates.insert(snapshot.id, snapshot.active);
    }
    scene_->setRouterRuntimeStates(std::move(runtimeStates));
    refreshRuntimeControls();
}

void MainWindow::onStartupProgress(const QString& stage, qint64 completed, qint64 total)
{
    if (!simulationStartPending_ || simulationStartCancelRequested_)
    {
        return;
    }
    const auto progress = total > 0 ? QStringLiteral("%1 / %2").arg(completed).arg(total) : QString{};
    const auto text = progress.isEmpty() ? stage : QStringLiteral("%1 · %2").arg(stage, progress);
    simulationStatusLabel_->setText(QStringLiteral("● 正在启动"));
    convergenceStateLabel_->setText(QStringLiteral("当前：%1").arg(text));
    statusBar()->showMessage(text);
}

void MainWindow::onStartupCancelled()
{
    if (!simulationStartPending_)
    {
        return;
    }
    simulationStartPending_ = false;
    simulationStartCancelRequested_ = false;
    endEventRun(false);
    simulationStatusLabel_->setText(QStringLiteral("● 已停止"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#6c757d"));
    scene_->setEditable(true);
    convergenceStateLabel_->setText(QStringLiteral("当前：已取消启动"));
    statusBar()->showMessage(QStringLiteral("已取消仿真启动"), 4000);
    updateEditorActions();
}

void MainWindow::onRibSnapshot(const RibSnapshot& snapshot)
{
    snapshotRequestPending_ = false;
    if (snapshot.router != routerCombo_->currentText())
    {
        if (snapshotRefreshNeeded_)
        {
            scheduleSelectedRouterSnapshot();
        }
        return;
    }
    currentRib_ = snapshot;
    populateRibTables(snapshot);
    if (snapshotRefreshNeeded_)
    {
        scheduleSelectedRouterSnapshot();
    }
}

void MainWindow::onPeerSnapshots(const QString& routerId, QVector<PeerSnapshot> snapshots)
{
    if (routerId == routerCombo_->currentText())
    {
        populatePeerTable(snapshots);
    }
}

void MainWindow::onRoutingStateChanged()
{
    scheduleSelectedRouterSnapshot();
}

void MainWindow::onPathReady(const QString& routerId, const QString& prefix, const QStringList& path)
{
    if (routerId == routerCombo_->currentText() && prefix == bestRoutesModel_->prefixAt(ribTable_->currentIndex().row()))
    {
        scene_->highlightPath(path);
    }
}

void MainWindow::onRouterRuntimeState(const QString& routerId, bool enabled)
{
    if (runtimeRouters_.contains(routerId))
    {
        runtimeRouters_[routerId].active = enabled;
    }
    scene_->setRouterRuntimeState(routerId, enabled);
    refreshRuntimeControls();
}

void MainWindow::onLinkRuntimeState(const QString& a, const QString& b, bool enabled)
{
    runtimeLinks_.insert(Topology::edgeKey(a, b), enabled);
    scene_->setLinkRuntimeState(a, b, enabled);
    refreshRuntimeControls();
}

void MainWindow::onEngineError(const QString& message)
{
    if (simulationStartPending_)
    {
        simulationStartPending_ = false;
        endEventRun(true);
        updateEditorActions();
        simulationStatusLabel_->setText(QStringLiteral("● 启动失败"));
        simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#b02a37"));
        convergenceStateLabel_->setText(QStringLiteral("当前：启动失败"));
        scene_->setEditable(true);
    }
    QMessageBox::critical(this, QStringLiteral("仿真错误"), message);
}

void MainWindow::selectedRouterChanged()
{
    requestSelectedRouterSnapshot();
    refreshRuntimeControls();
    scene_->highlightPath({});
}

void MainWindow::selectedLinkChanged()
{
    refreshRuntimeControls();
}

void MainWindow::sceneSelectionChanged(const QString& routerId, const QString& linkA, const QString& linkB)
{
    if (!routerId.isEmpty())
    {
        routerCombo_->setCurrentText(routerId);
    }
    if (!linkA.isEmpty())
    {
        const auto key = Topology::edgeKey(linkA, linkB);
        const auto index = linkCombo_->findData(key);
        if (index >= 0)
        {
            linkCombo_->setCurrentIndex(index);
        }
    }
}

void MainWindow::toggleSelectedRouter()
{
    const auto id = routerCombo_->currentText();
    if (!simulationRunning_ || id.isEmpty() || !runtimeRouters_.contains(id))
    {
        return;
    }
    const auto target = !runtimeRouters_.value(id).active;
    QMetaObject::invokeMethod(engine_, [engine = engine_, id, target] { engine->setRouterState(id, target); }, Qt::QueuedConnection);
}

void MainWindow::toggleSelectedLink()
{
    if (!simulationRunning_ || linkCombo_->currentIndex() < 0)
    {
        return;
    }
    const auto index = linkCombo_->currentIndex();
    const auto a = linkCombo_->itemData(index, Qt::UserRole + 1).toString();
    const auto b = linkCombo_->itemData(index, Qt::UserRole + 2).toString();
    const auto key = Topology::edgeKey(a, b);
    const auto initial = topology_.findLink(a, b) ? topology_.findLink(a, b)->enabled : false;
    const auto target = !runtimeLinks_.value(key, initial);
    QMetaObject::invokeMethod(engine_, [engine = engine_, a, b, target] { engine->setLinkState(a, b, target); }, Qt::QueuedConnection);
}

void MainWindow::advertisePrefix()
{
    const auto router = routerCombo_->currentText();
    const auto prefix = prefixEdit_->text().trimmed();
    if (!simulationRunning_ || router.isEmpty() || prefix.isEmpty())
    {
        return;
    }
    QMetaObject::invokeMethod(
        engine_, [engine = engine_, router, prefix] { engine->originatePrefix(router, prefix); }, Qt::QueuedConnection);
}

void MainWindow::withdrawPrefix()
{
    const auto router = routerCombo_->currentText();
    const auto prefix = prefixEdit_->text().trimmed();
    if (!simulationRunning_ || router.isEmpty() || prefix.isEmpty())
    {
        return;
    }
    QMetaObject::invokeMethod(
        engine_, [engine = engine_, router, prefix] { engine->withdrawPrefix(router, prefix); }, Qt::QueuedConnection);
}

void MainWindow::highlightSelectedRoute()
{
    const auto rows = ribTable_->selectionModel()->selectedRows();
    if (rows.isEmpty())
    {
        scene_->highlightPath({});
        return;
    }
    const auto prefix = bestRoutesModel_->prefixAt(rows.front().row());
    const auto router = routerCombo_->currentText();
    scene_->highlightPath({});
    if (router.isEmpty() || prefix.isEmpty())
    {
        return;
    }
    QMetaObject::invokeMethod(engine_, [engine = engine_, router, prefix] { engine->requestPath(router, prefix); }, Qt::QueuedConnection);
}

void MainWindow::showEventDetails(const QModelIndex& proxyIndex)
{
    const auto sourceIndex = eventProxy_->mapToSource(proxyIndex);
    const auto* event = eventModel_->eventAt(sourceIndex.row());
    if (!event)
    {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("事件 #%1").arg(event->id));
    dialog->resize(650, 480);
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QTextEdit(dialog);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(QString::fromUtf8(QJsonDocument(EventStore::eventToJson(*event)).toJson(QJsonDocument::Indented)));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(text);
    layout->addWidget(buttons);
    dialog->show();
}

void MainWindow::enqueueStoredEvents(quint64 runSerial, QVector<SimulationEvent> events)
{
    if (runSerial != eventRunSerial_ || events.isEmpty() || !historyDatabasePath_.isEmpty())
    {
        return;
    }
    const auto filter = eventFilterEdit_->text();
    for (const auto& event : events)
    {
        const auto message = event.event == QStringLiteral("message_received");
        const auto matched = EventTableModel::matchesFilter(event, filter);
        ++eventTotalCount_;
        if (message)
        {
            ++messageTotalCount_;
        }
        if (eventFilteredCountKnown_ && matched)
        {
            ++eventFilteredCount_;
            if (message)
            {
                ++messageFilteredCount_;
            }
        }
        if (liveCountDeltaActive_)
        {
            ++liveDeltaEventTotal_;
            if (matched)
            {
                ++liveDeltaEventFiltered_;
            }
            if (message)
            {
                ++liveDeltaMessageTotal_;
                if (matched)
                {
                    ++liveDeltaMessageFiltered_;
                }
            }
        }
    }
    updateEventCountLabel();
    appendConvergenceEvents(events);
    const auto capacity = EventTableModel::liveCapacity();
    if (events.size() >= capacity)
    {
        events.remove(0, events.size() - capacity);
        pendingUiEvents_ = std::move(events);
    }
    else
    {
        const auto overflow = pendingUiEvents_.size() + events.size() - capacity;
        if (overflow > 0)
        {
            pendingUiEvents_.remove(0, overflow);
        }
        for (auto& event : events)
        {
            pendingUiEvents_.append(std::move(event));
        }
    }
    if (!uiEventDrainTimer_->isActive())
    {
        uiEventDrainTimer_->start(0);
    }
}

void MainWindow::appendConvergenceEvents(const QVector<SimulationEvent>& events)
{
    for (const auto& event : events)
    {
        if (event.event != QStringLiteral("converged"))
        {
            continue;
        }
        const auto durationMs = nonNegativeDetail(event, QStringLiteral("duration_ms"));
        if (!durationMs)
        {
            continue;
        }
        const auto sequence =
            positiveDetail(event, QStringLiteral("convergence_sequence")).value_or(static_cast<quint64>(convergenceTable_->rowCount() + 1));
        appendConvergenceRecord(sequence, event.details.value(QStringLiteral("trigger_event")),
                                event.details.value(QStringLiteral("trigger_context")), event.timestamp, *durationMs,
                                unsignedDetail(event, QStringLiteral("bgp_message_count")));
    }
}

void MainWindow::appendConvergenceRecord(quint64 sequence, const QString& triggerEvent, const QString& triggerContext,
                                         const QDateTime& completedAt, qint64 durationMs, std::optional<quint64> bgpMessageCount)
{
    if (!completedAt.isValid())
    {
        return;
    }
    const auto row = convergenceTable_->rowCount();
    convergenceTable_->insertRow(row);
    convergenceTable_->setItem(row, 0, tableItem(QString::number(sequence)));
    auto* triggerItem = tableItem(convergenceTriggerText(triggerEvent, triggerContext), Qt::AlignLeft | Qt::AlignVCenter);
    triggerItem->setToolTip(QStringLiteral("事件名称：%1").arg(triggerEvent.isEmpty() ? QStringLiteral("未知") : triggerEvent));
    convergenceTable_->setItem(row, 1, triggerItem);
    convergenceTable_->setItem(row, 2, tableItem(completedAt.addMSecs(-durationMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    convergenceTable_->setItem(row, 3, tableItem(completedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    auto* durationItem = tableItem(durationText(durationMs));
    durationItem->setData(Qt::UserRole, durationMs);
    durationItem->setToolTip(QStringLiteral("%1 ms（包含收敛静默窗口）").arg(durationMs));
    convergenceTable_->setItem(row, 4, durationItem);
    auto* messageCountItem = tableItem(bgpMessageCount ? QString::number(*bgpMessageCount) : QStringLiteral("—"));
    if (bgpMessageCount)
    {
        messageCountItem->setData(Qt::UserRole, QVariant::fromValue(*bgpMessageCount));
        messageCountItem->setToolTip(QStringLiteral("本轮收敛共收到 %1 条 BGP 报文（event=message_received）").arg(*bgpMessageCount));
    }
    else
    {
        messageCountItem->setToolTip(QStringLiteral("该历史记录未保存 BGP 报文数量"));
    }
    convergenceTable_->setItem(row, 5, messageCountItem);
    convergenceCountLabel_->setText(QStringLiteral("已记录 %1 次").arg(convergenceTable_->rowCount()));
    convergenceStateLabel_->setText(QStringLiteral("当前：已收敛 · 触发：%1 · 最近 %2")
                                        .arg(convergenceTriggerText(triggerEvent, triggerContext), durationText(durationMs)));
    convergenceTable_->scrollToBottom();
}

void MainWindow::rebuildConvergenceHistory(const QVector<SimulationEvent>& events, qint64 totalCount)
{
    clearConvergenceHistory(QStringLiteral("当前：正在读取历史记录"));
    QDateTime inferredStart;
    QString inferredTriggerEvent;
    QString inferredTriggerContext;
    quint64 fallbackSequence = 0;
    for (const auto& event : events)
    {
        if (event.event == QStringLiteral("simulation_started"))
        {
            inferredStart = event.timestamp;
            inferredTriggerEvent = event.event;
            inferredTriggerContext = convergenceEventContext(event);
            continue;
        }
        if (event.event == QStringLiteral("converged"))
        {
            auto durationMs = nonNegativeDetail(event, QStringLiteral("duration_ms"));
            if (!durationMs && inferredStart.isValid() && event.timestamp.isValid())
            {
                durationMs = std::max<qint64>(0, inferredStart.msecsTo(event.timestamp));
            }
            if (durationMs)
            {
                const auto sequence = positiveDetail(event, QStringLiteral("convergence_sequence")).value_or(fallbackSequence + 1);
                fallbackSequence = std::max(fallbackSequence, sequence);
                const auto triggerEvent = event.details.value(QStringLiteral("trigger_event"), inferredTriggerEvent);
                const auto triggerContext = event.details.value(QStringLiteral("trigger_context"),
                                                                triggerEvent == inferredTriggerEvent ? inferredTriggerContext : QString{});
                appendConvergenceRecord(sequence, triggerEvent, triggerContext, event.timestamp, *durationMs,
                                        unsignedDetail(event, QStringLiteral("bgp_message_count")));
            }
            inferredStart = {};
            inferredTriggerEvent.clear();
            inferredTriggerContext.clear();
            continue;
        }
        if (!inferredStart.isValid() && event.event != QStringLiteral("simulation_stopped"))
        {
            inferredStart = event.timestamp;
            inferredTriggerEvent = event.event;
            inferredTriggerContext = convergenceEventContext(event);
        }
    }
    const auto displayedCount = convergenceTable_->rowCount();
    convergenceCountLabel_->setText(QStringLiteral("显示 %1 / 总计 %2 次").arg(displayedCount).arg(totalCount));
    convergenceStateLabel_->setText(totalCount > displayedCount
                                        ? QStringLiteral("当前：已载入最近 %1 次收敛记录（总计 %2 次，较早记录未显示）")
                                              .arg(displayedCount)
                                              .arg(totalCount)
                                        : QStringLiteral("当前：已载入全部 %1 次收敛记录").arg(totalCount));
}

void MainWindow::clearConvergenceHistory(const QString& stateText)
{
    convergenceTable_->setRowCount(0);
    convergenceCountLabel_->setText(QStringLiteral("已记录 0 次"));
    convergenceStateLabel_->setText(stateText);
}

void MainWindow::drainUiEventQueue()
{
    constexpr qsizetype eventsPerTurn = 512;
    const auto count = std::min(eventsPerTurn, pendingUiEvents_.size());
    if (count <= 0)
    {
        return;
    }
    QVector<SimulationEvent> batch;
    batch.reserve(count);
    for (qsizetype index = 0; index < count; ++index)
    {
        batch.append(std::move(pendingUiEvents_[index]));
    }
    pendingUiEvents_.remove(0, count);
    eventModel_->appendEvents(std::move(batch));
    if (followEventsCheck_->isChecked())
    {
        eventView_->scrollToBottom();
    }
    if (!pendingUiEvents_.isEmpty())
    {
        uiEventDrainTimer_->start(16);
    }
}

void MainWindow::setTopology(Topology topology, const QString& path)
{
    topology_ = std::move(topology);
    topologyPath_ = path;
    runtimeRouters_.clear();
    runtimeLinks_.clear();
    currentRib_ = {};
    scene_->clearRuntimeState();
    scene_->setTopology(&topology_);
    refreshTopologySelectors();
    updateWindowTitle();
    QTimer::singleShot(0, this, [this] { fitAction_->trigger(); });
}

void MainWindow::setDirty(bool dirty)
{
    dirty_ = dirty;
    updateEditorActions();
    updateWindowTitle();
}

bool MainWindow::maybeSave()
{
    if (!dirty_)
    {
        return true;
    }
    const auto choice = QMessageBox::warning(this, QStringLiteral("未保存的更改"), QStringLiteral("当前拓扑尚未保存。是否先保存？"),
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
    {
        return false;
    }
    return choice == QMessageBox::Discard || saveTopology();
}

void MainWindow::updateWindowTitle()
{
    const auto file = topologyPath_.isEmpty() ? QStringLiteral("未命名") : QFileInfo(topologyPath_).fileName();
    setWindowTitle(QStringLiteral("%1%2 — BgpTester").arg(dirty_ ? QStringLiteral("*") : QString{}, file));
}

void MainWindow::updateEditorActions()
{
    const auto editable =
        !closing_ && !simulationRunning_ && !simulationStartPending_ && !historyLoadInProgress_ && !topologyLoadInProgress_;
    const auto runtimeControlsEnabled = !closing_ && simulationRunning_;
    const auto stopEnabled = !closing_ && (simulationRunning_ || simulationStartPending_);
    for (auto* action : {newAction_, openAction_, saveAsAction_, settingsAction_, batchTopologyAction_, selectModeAction_, addRouterAction_,
                         addLinkAction_, deleteAction_})
    {
        action->setEnabled(editable);
    }
    saveAction_->setEnabled(editable && dirty_);
    openHistoryAction_->setEnabled(editable);
    startAction_->setEnabled(editable);
    stopAction_->setEnabled(stopEnabled);
    routerToggleButton_->setEnabled(runtimeControlsEnabled);
    linkBrowseButton_->setEnabled(!closing_ && linkCombo_->count() > 0);
    linkToggleButton_->setEnabled(runtimeControlsEnabled);
    prefixEdit_->setEnabled(runtimeControlsEnabled);
    eventFilterEdit_->setEnabled(!closing_ && !historyLoadInProgress_ && !simulationStartPending_);
    if (scene_)
    {
        scene_->setEditable(editable);
    }
    updateHistoryControls();
}

void MainWindow::refreshTopologySelectors()
{
    const auto oldRouter = routerCombo_->currentText();
    routerCombo_->blockSignals(true);
    routerListModel_->setTopology(&topology_);
    const auto routerIndex = oldRouter.isEmpty() ? -1 : routerCombo_->findText(oldRouter);
    routerCombo_->setCurrentIndex(routerIndex >= 0 ? routerIndex : (routerCombo_->count() > 0 ? 0 : -1));
    routerCombo_->blockSignals(false);

    const auto oldLink = linkCombo_->currentData().toString();
    linkCombo_->blockSignals(true);
    linkListModel_->setTopology(&topology_);
    const auto linkIndex = oldLink.isEmpty() ? -1 : linkCombo_->findData(oldLink);
    linkCombo_->setCurrentIndex(linkIndex >= 0 ? linkIndex : (linkCombo_->count() > 0 ? 0 : -1));
    linkCombo_->blockSignals(false);
    linkBrowseButton_->setEnabled(!closing_ && linkCombo_->count() > 0);
    refreshRuntimeControls();
}

void MainWindow::requestSelectedRouterSnapshot()
{
    const auto id = routerCombo_->currentText();
    if (!simulationRunning_ || id.isEmpty())
    {
        snapshotRequestPending_ = false;
        snapshotRefreshNeeded_ = false;
        currentRib_ = {};
        currentRib_.router = id;
        populateRibTables(currentRib_);
        populatePeerTable({});
        return;
    }
    if (snapshotRequestPending_)
    {
        snapshotRefreshNeeded_ = true;
        return;
    }
    snapshotRequestPending_ = true;
    snapshotRefreshNeeded_ = false;
    QMetaObject::invokeMethod(engine_, [engine = engine_, id] { engine->requestRouterSnapshot(id); }, Qt::QueuedConnection);
}

void MainWindow::scheduleSelectedRouterSnapshot()
{
    if (!simulationRunning_)
    {
        return;
    }
    snapshotRefreshNeeded_ = true;
    if (!snapshotRequestPending_ && !ribRefreshTimer_->isActive())
    {
        ribRefreshTimer_->start();
    }
}

void MainWindow::populateRibTables(const RibSnapshot& snapshot)
{
    if (inspectorTabs_->currentIndex() == 0)
    {
        bestRoutesModel_->setSnapshot(snapshot);
        allRoutesModel_->setSnapshot({});
    }
    else if (inspectorTabs_->currentIndex() == 1)
    {
        bestRoutesModel_->setSnapshot({});
        allRoutesModel_->setSnapshot(snapshot);
    }
}

void MainWindow::populatePeerTable(const QVector<PeerSnapshot>& snapshots)
{
    peerTable_->setRowCount(snapshots.size());
    for (int row = 0; row < snapshots.size(); ++row)
    {
        const auto& peer = snapshots.at(row);
        peerTable_->setItem(row, 0, tableItem(peer.id));
        peerTable_->setItem(row, 1, tableItem(QString::number(peer.remoteAsn)));
        peerTable_->setItem(row, 2, tableItem(toString(peer.sessionType)));
        peerTable_->setItem(row, 3, tableItem(relationshipDisplayName(peer.relationship)));
        peerTable_->setItem(row, 4, tableItem(toString(peer.state)));
        peerTable_->setItem(row, 5, tableItem(peer.rrClient ? QStringLiteral("是") : QStringLiteral("否")));
        peerTable_->setItem(row, 6, tableItem(QStringLiteral("%1 ms").arg(peer.mraiMs)));
    }
}

void MainWindow::refreshRuntimeControls()
{
    const auto router = routerCombo_->currentText();
    if (!simulationRunning_ || !runtimeRouters_.contains(router))
    {
        routerStateLabel_->setText(QStringLiteral("—"));
        routerToggleButton_->setEnabled(false);
    }
    else
    {
        const auto active = runtimeRouters_.value(router).active;
        routerStateLabel_->setText(active ? QStringLiteral("运行中") : QStringLiteral("已关闭"));
        routerToggleButton_->setText(active ? QStringLiteral("关闭节点") : QStringLiteral("恢复节点"));
        routerToggleButton_->setEnabled(true);
    }

    if (!simulationRunning_ || linkCombo_->currentIndex() < 0)
    {
        linkStateLabel_->setText(QStringLiteral("—"));
        linkToggleButton_->setEnabled(false);
    }
    else
    {
        const auto index = linkCombo_->currentIndex();
        const auto a = linkCombo_->itemData(index, Qt::UserRole + 1).toString();
        const auto b = linkCombo_->itemData(index, Qt::UserRole + 2).toString();
        const auto key = Topology::edgeKey(a, b);
        const auto* link = topology_.findLink(a, b);
        const auto enabled = runtimeLinks_.value(key, link && link->enabled);
        linkStateLabel_->setText(enabled ? QStringLiteral("已连接") : QStringLiteral("已断开"));
        linkToggleButton_->setText(enabled ? QStringLiteral("断开链路") : QStringLiteral("恢复链路"));
        linkToggleButton_->setEnabled(true);
    }
}

void MainWindow::endEventRun(bool blocking)
{
    if (!eventStore_ || !eventStoreThread_.isRunning())
    {
        return;
    }
    QMetaObject::invokeMethod(eventStore_, &EventStore::endRun, blocking ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
}

void MainWindow::flushEventStore()
{
    if (eventStore_ && eventStoreThread_.isRunning())
    {
        QMetaObject::invokeMethod(eventStore_, &EventStore::flush, Qt::QueuedConnection);
    }
}

Topology MainWindow::starterTopology()
{
    Topology topology;
    topology.simulation.name = QStringLiteral("quick-lab");
    RouterConfig r1{.id = QStringLiteral("R1"),
                    .routerId = QStringLiteral("10.0.0.1"),
                    .asn = 65001,
                    .clusterId = QStringLiteral("10.0.0.1"),
                    .originatedPrefixes = {QStringLiteral("10.1.0.0/24")},
                    .position = QPointF(180, 220),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    RouterConfig r2{.id = QStringLiteral("R2"),
                    .routerId = QStringLiteral("10.0.0.2"),
                    .asn = 65002,
                    .clusterId = QStringLiteral("10.0.0.2"),
                    .originatedPrefixes = {QStringLiteral("10.2.0.0/24")},
                    .position = QPointF(450, 220),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    topology.routers.insert(r1.id, r1);
    topology.routers.insert(r2.id, r2);
    topology.links.append(LinkConfig{.a = r1.id, .b = r2.id, .delayMs = 10});
    return topology;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (closing_)
    {
        event->accept();
        return;
    }
    if (!maybeSave())
    {
        event->ignore();
        return;
    }
    closing_ = true;
    if (simulationStartPending_)
    {
        simulationStartCancelRequested_ = true;
        engine_->requestStartupCancellation();
    }
    stopTopologyLoad();
    ++historyQueryGeneration_;
    ++liveCountRequestId_;
    if (eventFilterTimer_)
    {
        eventFilterTimer_->stop();
    }
    updateEditorActions();
    stopAndWaitForQueryThreads();
    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("main/state"), saveState());
    if (simulationRunning_)
    {
        QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::BlockingQueuedConnection);
    }
    endEventRun(true);
    event->accept();
}

} // namespace bgptester
