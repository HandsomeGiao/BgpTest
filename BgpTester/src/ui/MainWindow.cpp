#include "ui/MainWindow.hpp"

#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "ui/Dialogs.hpp"
#include "ui/EventTableModel.hpp"
#include "ui/RibTableModels.hpp"
#include "ui/TopologyScene.hpp"

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
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressDialog>
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
#include <memory>
#include <optional>

namespace bgptester
{
namespace
{

struct HistoryLoadState
{
    QVector<SimulationEvent> events;
    QString error;
    std::atomic<qint64> loadedRows{0};
    std::atomic<qint64> totalRows{0};
    std::atomic_bool cancelRequested{false};
};

QTableWidgetItem* tableItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
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
    const auto lastTopology = settings.value(QStringLiteral("files/lastTopology")).toString();
    if (lastTopology.isEmpty() || !QFileInfo::exists(lastTopology) || !openTopologyPath(lastTopology, false))
    {
        setTopology(starterTopology());
        setDirty(false);
    }
}

MainWindow::~MainWindow()
{
    if (engineThread_.isRunning())
    {
        if (simulationRunning_)
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
    routerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
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
    peerTable_->setColumnCount(6);
    peerTable_->setHorizontalHeaderLabels({QStringLiteral("邻居"), QStringLiteral("Remote AS"), QStringLiteral("会话"),
                                           QStringLiteral("状态"), QStringLiteral("RR Client"), QStringLiteral("MRAI")});
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
    linkCombo_ = new QComboBox(linkBox);
    connect(linkCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::selectedLinkChanged);
    auto* linkStateRow = new QHBoxLayout;
    linkStateLabel_ = new QLabel(QStringLiteral("—"), linkBox);
    linkToggleButton_ = new QToolButton(linkBox);
    linkToggleButton_->setText(QStringLiteral("切换"));
    connect(linkToggleButton_, &QToolButton::clicked, this, &MainWindow::toggleSelectedLink);
    linkStateRow->addWidget(linkStateLabel_, 1);
    linkStateRow->addWidget(linkToggleButton_);
    linkLayout->addWidget(linkCombo_);
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
    eventCountLabel_->setMinimumWidth(125);
    followEventsCheck_ = new QCheckBox(QStringLiteral("跟随实时"), eventPage);
    followEventsCheck_->setChecked(true);
    auto* clearButton = new QPushButton(QStringLiteral("清空表格"), eventPage);
    auto* historyButton = new QPushButton(QStringLiteral("打开历史…"), eventPage);
    connect(clearButton, &QPushButton::clicked, eventModel_, &EventTableModel::clear);
    connect(historyButton, &QPushButton::clicked, this, &MainWindow::openHistory);
    filterRow->addWidget(eventFilterEdit_, 1);
    filterRow->addWidget(eventCountLabel_);
    filterRow->addWidget(followEventsCheck_);
    filterRow->addWidget(clearButton);
    filterRow->addWidget(historyButton);
    layout->addLayout(filterRow);

    eventProxy_ = new QSortFilterProxyModel(this);
    eventProxy_->setSourceModel(eventModel_);
    eventProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    eventProxy_->setFilterKeyColumn(-1);
    eventProxy_->setFilterRole(Qt::UserRole);
    eventProxy_->setSortRole(Qt::DisplayRole);
    connect(eventFilterEdit_, &QLineEdit::textChanged, eventProxy_, &QSortFilterProxyModel::setFilterFixedString);
    const auto updateEventCount = [this]
    { eventCountLabel_->setText(QStringLiteral("显示 %1 / 共 %2 条").arg(eventProxy_->rowCount()).arg(eventModel_->rowCount())); };
    connect(eventProxy_, &QAbstractItemModel::rowsInserted, this, updateEventCount);
    connect(eventProxy_, &QAbstractItemModel::rowsRemoved, this, updateEventCount);
    connect(eventProxy_, &QAbstractItemModel::modelReset, this, updateEventCount);
    connect(eventProxy_, &QAbstractItemModel::layoutChanged, this, updateEventCount);
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

    convergenceTable_ = new QTableWidget(0, 5, convergencePage);
    convergenceTable_->setObjectName(QStringLiteral("convergenceHistoryTable"));
    convergenceTable_->setHorizontalHeaderLabels({QStringLiteral("轮次"), QStringLiteral("触发事件"), QStringLiteral("开始时间"),
                                                  QStringLiteral("完成时间"), QStringLiteral("持续时间")});
    configureDataTable(convergenceTable_);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    convergenceTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    convergenceLayout->addWidget(convergenceTable_, 1);
    auto* convergenceNote = new QLabel(QStringLiteral("持续时间包含配置的收敛静默窗口。"), convergencePage);
    convergenceNote->setStyleSheet(QStringLiteral("color:#6c757d"));
    convergenceLayout->addWidget(convergenceNote);
    monitorTabs->addTab(convergencePage, QStringLiteral("收敛时间"));

    dock->setWidget(monitorTabs);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    viewMenu_->addAction(dock->toggleViewAction());
    resizeDocks({dock}, {280}, Qt::Vertical);
}

void MainWindow::connectEngine()
{
    engine_ = new SimulationEngine;
    engine_->moveToThread(&engineThread_);
    connect(&engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
    connect(engine_, &SimulationEngine::runningChanged, this, &MainWindow::onRunningChanged);
    connect(engine_, &SimulationEngine::convergenceChanged, this, &MainWindow::onConvergenceChanged);
    connect(engine_, &SimulationEngine::statsChanged, this, &MainWindow::onStatsChanged);
    connect(engine_, &SimulationEngine::eventsGenerated, eventStore_, &EventStore::enqueueEvents, Qt::DirectConnection);
    connect(engine_, &SimulationEngine::routerSnapshotsChanged, this, &MainWindow::onRouterSnapshots);
    connect(engine_, &SimulationEngine::ribSnapshotReady, this, &MainWindow::onRibSnapshot);
    connect(engine_, &SimulationEngine::peersSnapshotReady, this, &MainWindow::onPeerSnapshots);
    connect(engine_, &SimulationEngine::ribChanged, this, &MainWindow::onRibChanged);
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
    QString error;
    const auto topology = Topology::load(path, &error);
    if (!topology)
    {
        if (showErrors)
        {
            QMessageBox::critical(this, QStringLiteral("无法打开拓扑"), error);
        }
        return false;
    }
    setTopology(*topology, QFileInfo(path).absoluteFilePath());
    setDirty(false);
    QSettings().setValue(QStringLiteral("files/lastTopology"), topologyPath_);
    statusBar()->showMessage(QStringLiteral("已加载 %1").arg(topologyPath_), 4000);
    return true;
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
    if (historyLoadInProgress_)
    {
        return;
    }

    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 BMP SQLite 日志"), QDir::currentPath(),
                                                   QStringLiteral("SQLite database (*.sqlite *.db);;所有文件 (*.*)"));
    if (path.isEmpty())
    {
        return;
    }

    historyLoadInProgress_ = true;
    openHistoryAction_->setEnabled(false);
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
        [state, path]
        {
            state->events = EventStore::readDatabase(path, 0, &state->error,
                                                     [state](qsizetype loaded, qsizetype total)
                                                     {
                                                         state->loadedRows.store(loaded, std::memory_order_relaxed);
                                                         state->totalRows.store(total, std::memory_order_relaxed);
                                                         return !state->cancelRequested.load(std::memory_order_relaxed);
                                                     });
        });
    connect(loadThread, &QThread::finished, this,
            [this, state, path, progressDialog]
            {
                historyLoadInProgress_ = false;
                openHistoryAction_->setEnabled(!simulationRunning_);
                progressDialog->setRange(0, 1000);
                progressDialog->setValue(1000);

                if (state->cancelRequested.load(std::memory_order_relaxed))
                {
                    progressDialog->deleteLater();
                    statusBar()->showMessage(QStringLiteral("已取消加载历史 BMP 日志"), 4000);
                    return;
                }

                if (!state->error.isEmpty())
                {
                    progressDialog->deleteLater();
                    QMessageBox::critical(this, QStringLiteral("无法读取日志"), state->error);
                    return;
                }

                const auto eventCount = state->events.size();
                progressDialog->setLabelText(QStringLiteral("正在更新日志表格…"));
                eventProxy_->sort(-1);
                rebuildConvergenceHistory(state->events);
                eventModel_->setEvents(std::move(state->events));
                followEventsCheck_->setChecked(false);
                logPathLabel_->setText(QStringLiteral("历史：%1").arg(QDir::toNativeSeparators(path)));
                statusBar()->showMessage(QStringLiteral("已载入 %1 条历史 BMP 日志").arg(eventCount), 4000);
                progressDialog->deleteLater();
            });
    connect(loadThread, &QThread::finished, loadThread, &QObject::deleteLater);
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
    QStringList appliedOperations;

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
        const auto targetDescription = selectedRoutersOnly ? QStringLiteral("选中的 %1 台路由器").arg(targetRouters.size())
                                                           : QStringLiteral("全部 %1 台路由器").arg(targetRouters.size());
        appliedOperations.append(QStringLiteral("已将%1的种类设置为 %2").arg(targetDescription, pluginId));
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

    if (changedRouters > 0 || changedLinks > 0)
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
    LinkDialog dialog(config, this);
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
    LinkDialog dialog(*link, this);
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
    const auto problems = topology_.validate();
    if (!problems.isEmpty())
    {
        QMessageBox::critical(this, QStringLiteral("拓扑无效"), problems.join(u'\n'));
        return;
    }
    QString error;
    if (!beginEventRun(&error))
    {
        QMessageBox::critical(this, QStringLiteral("无法创建日志"), error);
        return;
    }
    eventModel_->clear();
    clearConvergenceHistory(QStringLiteral("当前：等待启动"));
    pendingUiEvents_.clear();
    followEventsCheck_->setChecked(true);
    runtimeRouters_.clear();
    runtimeLinks_.clear();
    scene_->clearRuntimeState();
    startAction_->setEnabled(false);
    simulationStartPending_ = true;
    const auto copy = topology_;
    QMetaObject::invokeMethod(engine_, [engine = engine_, copy] { engine->startSimulation(copy); }, Qt::QueuedConnection);
}

void MainWindow::stopSimulation()
{
    stopAction_->setEnabled(false);
    QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::QueuedConnection);
}

void MainWindow::onRunningChanged(bool running)
{
    simulationStartPending_ = false;
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
    for (const auto& snapshot : snapshots)
    {
        runtimeRouters_.insert(snapshot.id, snapshot);
        scene_->setRouterRuntimeState(snapshot.id, snapshot.active);
    }
    refreshRuntimeControls();
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

void MainWindow::onRibChanged(const QString& routerId)
{
    if (routerId == routerCombo_->currentText())
    {
        scheduleSelectedRouterSnapshot();
    }
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
    if (runSerial != eventRunSerial_ || events.isEmpty())
    {
        return;
    }
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
                                event.details.value(QStringLiteral("trigger_context")), event.timestamp, *durationMs);
    }
}

void MainWindow::appendConvergenceRecord(quint64 sequence, const QString& triggerEvent, const QString& triggerContext,
                                         const QDateTime& completedAt, qint64 durationMs)
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
    convergenceCountLabel_->setText(QStringLiteral("已记录 %1 次").arg(convergenceTable_->rowCount()));
    convergenceStateLabel_->setText(QStringLiteral("当前：已收敛 · 触发：%1 · 最近 %2")
                                        .arg(convergenceTriggerText(triggerEvent, triggerContext), durationText(durationMs)));
    convergenceTable_->scrollToBottom();
}

void MainWindow::rebuildConvergenceHistory(const QVector<SimulationEvent>& events)
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
                appendConvergenceRecord(sequence, triggerEvent, triggerContext, event.timestamp, *durationMs);
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
    convergenceStateLabel_->setText(QStringLiteral("当前：已载入历史记录"));
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
    saveAction_->setEnabled(dirty_ && !simulationRunning_);
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
    const auto editable = !simulationRunning_;
    for (auto* action : {newAction_, openAction_, saveAsAction_, settingsAction_, batchTopologyAction_, selectModeAction_, addRouterAction_,
                         addLinkAction_, deleteAction_})
    {
        action->setEnabled(editable);
    }
    saveAction_->setEnabled(editable && dirty_);
    openHistoryAction_->setEnabled(editable);
    startAction_->setEnabled(editable);
    stopAction_->setEnabled(!editable);
    routerToggleButton_->setEnabled(!editable);
    linkToggleButton_->setEnabled(!editable);
    prefixEdit_->setEnabled(!editable);
}

void MainWindow::refreshTopologySelectors()
{
    const auto oldRouter = routerCombo_->currentText();
    routerCombo_->blockSignals(true);
    routerCombo_->clear();
    routerCombo_->addItems(topology_.routers.keys());
    const auto routerIndex = routerCombo_->findText(oldRouter);
    if (routerIndex >= 0)
    {
        routerCombo_->setCurrentIndex(routerIndex);
    }
    routerCombo_->blockSignals(false);

    const auto oldLink = linkCombo_->currentData().toString();
    linkCombo_->blockSignals(true);
    linkCombo_->clear();
    for (const auto& link : topology_.links)
    {
        const auto key = Topology::edgeKey(link.a, link.b);
        linkCombo_->addItem(QStringLiteral("%1 ↔ %2").arg(link.a, link.b), key);
        const auto row = linkCombo_->count() - 1;
        linkCombo_->setItemData(row, link.a, Qt::UserRole + 1);
        linkCombo_->setItemData(row, link.b, Qt::UserRole + 2);
    }
    const auto linkIndex = linkCombo_->findData(oldLink);
    if (linkIndex >= 0)
    {
        linkCombo_->setCurrentIndex(linkIndex);
    }
    linkCombo_->blockSignals(false);
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
        peerTable_->setItem(row, 3, tableItem(toString(peer.state)));
        peerTable_->setItem(row, 4, tableItem(peer.rrClient ? QStringLiteral("是") : QStringLiteral("否")));
        peerTable_->setItem(row, 5, tableItem(QStringLiteral("%1 ms").arg(peer.mraiMs)));
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

bool MainWindow::beginEventRun(QString* error)
{
    bool started = false;
    quint64 workerRunSerial = 0;
    QString workerError;
    const auto settings = topology_.simulation;
    const auto invoked = QMetaObject::invokeMethod(
        eventStore_,
        [&]
        {
            started = eventStore_->beginRun(settings, &workerError);
            workerRunSerial = eventStore_->runSerial();
        },
        Qt::BlockingQueuedConnection);
    if (!invoked)
    {
        workerError = QStringLiteral("无法调用 BMP 事件持久化线程");
    }
    if (invoked && started)
    {
        eventRunSerial_ = workerRunSerial;
    }
    if (error)
    {
        *error = workerError;
    }
    return invoked && started;
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
