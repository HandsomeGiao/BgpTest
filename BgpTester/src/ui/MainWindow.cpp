#include "ui/MainWindow.hpp"

#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "ui/Dialogs.hpp"
#include "ui/EventTableModel.hpp"
#include "ui/TopologyScene.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
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
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace bgptester {
namespace {

QTableWidgetItem *tableItem(const QString &text,
                            Qt::Alignment alignment = Qt::AlignCenter) {
  auto *item = new QTableWidgetItem(text);
  item->setTextAlignment(alignment);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  return item;
}

QString asPathText(const QVector<quint32> &path) {
  QStringList values;
  for (const auto asn : path) {
    values.append(QString::number(asn));
  }
  return values.join(u' ');
}

void configureDataTable(QTableWidget *table) {
  table->setAlternatingRowColors(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setObjectName(QStringLiteral("BgpTesterMainWindow"));
  resize(1480, 900);
  setDockNestingEnabled(true);

  scene_ = new TopologyScene(this);
  view_ = new TopologyView(scene_, this);
  setCentralWidget(view_);
  eventStore_ = new EventStore(this);
  eventModel_ = new EventTableModel(this);

  buildActions();
  buildMenusAndToolbar();
  buildInspectorDock();
  buildEventDock();
  connectEngine();

  connect(scene_, &TopologyScene::createRouterRequested, this,
          &MainWindow::createRouter);
  connect(scene_, &TopologyScene::createLinkRequested, this,
          &MainWindow::createLink);
  connect(scene_, &TopologyScene::editRouterRequested, this,
          &MainWindow::editRouter);
  connect(scene_, &TopologyScene::editLinkRequested, this,
          &MainWindow::editLink);
  connect(scene_, &TopologyScene::topologyModified, this,
          &MainWindow::markDirty);
  connect(scene_, &TopologyScene::selectionContextChanged, this,
          &MainWindow::sceneSelectionChanged);
  connect(eventStore_, &EventStore::eventStored, eventModel_,
          &EventTableModel::appendEvent);
  connect(eventStore_, &EventStore::storeError, this,
          [this](const QString &message) {
            statusBar()->showMessage(message, 8000);
          });
  connect(eventStore_, &EventStore::pathsChanged, this,
          [this](const QString &, const QString &database) {
            logPathLabel_->setText(
                QStringLiteral("日志：%1").arg(QDir::toNativeSeparators(database)));
            logPathLabel_->setToolTip(database);
          });
  connect(eventModel_, &QAbstractItemModel::rowsInserted, this,
          [this] {
            if (followEventsCheck_->isChecked()) {
              eventView_->scrollToBottom();
            }
          });

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
  const auto lastTopology =
      settings.value(QStringLiteral("files/lastTopology")).toString();
  if (lastTopology.isEmpty() || !QFileInfo::exists(lastTopology) ||
      !openTopologyPath(lastTopology, false)) {
    setTopology(starterTopology());
    setDirty(false);
  }
}

MainWindow::~MainWindow() {
  if (engineThread_.isRunning()) {
    if (simulationRunning_) {
      QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation,
                                Qt::BlockingQueuedConnection);
    }
    engineThread_.quit();
    engineThread_.wait();
  }
  eventStore_->endRun();
}

void MainWindow::buildActions() {
  newAction_ = new QAction(QStringLiteral("新建"), this);
  newAction_->setShortcut(QKeySequence::New);
  connect(newAction_, &QAction::triggered, this, &MainWindow::newTopology);
  openAction_ = new QAction(QStringLiteral("打开拓扑…"), this);
  openAction_->setShortcut(QKeySequence::Open);
  connect(openAction_, &QAction::triggered, this, &MainWindow::openTopology);
  saveAction_ = new QAction(QStringLiteral("保存"), this);
  saveAction_->setShortcut(QKeySequence::Save);
  connect(saveAction_, &QAction::triggered, this, &MainWindow::saveTopology);
  saveAsAction_ = new QAction(QStringLiteral("另存为…"), this);
  saveAsAction_->setShortcut(QKeySequence::SaveAs);
  connect(saveAsAction_, &QAction::triggered, this,
          &MainWindow::saveTopologyAs);
  openHistoryAction_ = new QAction(QStringLiteral("打开 SQLite 日志…"), this);
  connect(openHistoryAction_, &QAction::triggered, this,
          &MainWindow::openHistory);
  settingsAction_ = new QAction(QStringLiteral("仿真设置…"), this);
  connect(settingsAction_, &QAction::triggered, this,
          &MainWindow::editSimulationSettings);

  modeGroup_ = new QActionGroup(this);
  modeGroup_->setExclusive(true);
  selectModeAction_ = new QAction(QStringLiteral("选择/移动"), this);
  selectModeAction_->setCheckable(true);
  selectModeAction_->setChecked(true);
  selectModeAction_->setShortcut(Qt::Key_V);
  addRouterAction_ = new QAction(QStringLiteral("添加路由器"), this);
  addRouterAction_->setCheckable(true);
  addRouterAction_->setShortcut(Qt::Key_R);
  addLinkAction_ = new QAction(QStringLiteral("添加链路"), this);
  addLinkAction_->setCheckable(true);
  addLinkAction_->setShortcut(Qt::Key_Q);
  modeGroup_->addAction(selectModeAction_);
  modeGroup_->addAction(addRouterAction_);
  modeGroup_->addAction(addLinkAction_);
  connect(selectModeAction_, &QAction::triggered, this,
          [this] { scene_->setMode(TopologyScene::Mode::Select); });
  connect(addRouterAction_, &QAction::triggered, this,
          [this] { scene_->setMode(TopologyScene::Mode::AddRouter); });
  connect(addLinkAction_, &QAction::triggered, this,
          [this] { scene_->setMode(TopologyScene::Mode::AddLink); });
  deleteAction_ = new QAction(QStringLiteral("删除所选"), this);
  deleteAction_->setShortcut(QKeySequence::Delete);
  connect(deleteAction_, &QAction::triggered, this,
          &MainWindow::deleteSelection);
  fitAction_ = new QAction(QStringLiteral("适合窗口"), this);
  fitAction_->setShortcut(Qt::Key_F);
  connect(fitAction_, &QAction::triggered, this, [this] {
    if (!scene_->items().isEmpty()) {
      view_->fitInView(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60),
                       Qt::KeepAspectRatio);
    }
  });

  startAction_ = new QAction(QStringLiteral("启动仿真"), this);
  startAction_->setShortcut(Qt::Key_F5);
  connect(startAction_, &QAction::triggered, this,
          &MainWindow::startSimulation);
  stopAction_ = new QAction(QStringLiteral("停止"), this);
  stopAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
  stopAction_->setEnabled(false);
  connect(stopAction_, &QAction::triggered, this,
          &MainWindow::stopSimulation);
}

void MainWindow::buildMenusAndToolbar() {
  auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
  fileMenu->addActions({newAction_, openAction_, saveAction_, saveAsAction_});
  fileMenu->addSeparator();
  fileMenu->addAction(openHistoryAction_);
  fileMenu->addSeparator();
  auto *exitAction = fileMenu->addAction(QStringLiteral("退出"));
  exitAction->setShortcut(QKeySequence::Quit);
  connect(exitAction, &QAction::triggered, this, &QWidget::close);
  auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
  editMenu->addActions({selectModeAction_, addRouterAction_, addLinkAction_,
                        deleteAction_});
  editMenu->addSeparator();
  editMenu->addAction(settingsAction_);
  auto *simulationMenu = menuBar()->addMenu(QStringLiteral("仿真(&S)"));
  simulationMenu->addActions({startAction_, stopAction_});
  auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
  viewMenu->addAction(fitAction_);

  auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
  toolbar->setObjectName(QStringLiteral("mainToolbar"));
  toolbar->setMovable(false);
  toolbar->addActions({newAction_, openAction_, saveAction_});
  toolbar->addSeparator();
  toolbar->addActions(
      {selectModeAction_, addRouterAction_, addLinkAction_, deleteAction_});
  toolbar->addAction(fitAction_);
  toolbar->addSeparator();
  toolbar->addActions({startAction_, stopAction_});
  toolbar->addSeparator();
  toolbar->addAction(settingsAction_);
}

void MainWindow::buildInspectorDock() {
  auto *dock = new QDockWidget(QStringLiteral("路由检查器"), this);
  dock->setObjectName(QStringLiteral("inspectorDock"));
  dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  auto *container = new QWidget(dock);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);
  auto *routerRow = new QHBoxLayout;
  routerRow->addWidget(new QLabel(QStringLiteral("路由器"), container));
  routerCombo_ = new QComboBox(container);
  routerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  routerRow->addWidget(routerCombo_, 1);
  connect(routerCombo_, &QComboBox::currentIndexChanged, this,
          &MainWindow::selectedRouterChanged);
  layout->addLayout(routerRow);

  auto *tabs = new QTabWidget(container);
  ribTable_ = new QTableWidget(tabs);
  ribTable_->setColumnCount(7);
  ribTable_->setHorizontalHeaderLabels(
      {QStringLiteral("前缀"), QStringLiteral("来源"),
       QStringLiteral("会话"), QStringLiteral("NEXT_HOP"),
       QStringLiteral("AS_PATH"), QStringLiteral("LOCAL_PREF"),
       QStringLiteral("MED")});
  configureDataTable(ribTable_);
  connect(ribTable_, &QTableWidget::itemSelectionChanged, this,
          &MainWindow::highlightSelectedRoute);
  tabs->addTab(ribTable_, QStringLiteral("最佳路由"));

  allRoutesTable_ = new QTableWidget(tabs);
  allRoutesTable_->setColumnCount(8);
  allRoutesTable_->setHorizontalHeaderLabels(
      {QStringLiteral("前缀"), QStringLiteral("RIB"),
       QStringLiteral("来源"), QStringLiteral("最佳"),
       QStringLiteral("NEXT_HOP"), QStringLiteral("AS_PATH"),
       QStringLiteral("LOCAL_PREF"), QStringLiteral("MED")});
  configureDataTable(allRoutesTable_);
  tabs->addTab(allRoutesTable_, QStringLiteral("全部路径"));

  peerTable_ = new QTableWidget(tabs);
  peerTable_->setColumnCount(6);
  peerTable_->setHorizontalHeaderLabels(
      {QStringLiteral("邻居"), QStringLiteral("Remote AS"),
       QStringLiteral("会话"), QStringLiteral("状态"),
       QStringLiteral("RR Client"), QStringLiteral("MRAI")});
  configureDataTable(peerTable_);
  tabs->addTab(peerTable_, QStringLiteral("邻居"));

  auto *control = new QWidget(tabs);
  auto *controlLayout = new QVBoxLayout(control);
  auto *nodeBox = new QGroupBox(QStringLiteral("节点状态"), control);
  auto *nodeLayout = new QHBoxLayout(nodeBox);
  routerStateLabel_ = new QLabel(QStringLiteral("—"), nodeBox);
  routerToggleButton_ = new QToolButton(nodeBox);
  routerToggleButton_->setText(QStringLiteral("切换"));
  connect(routerToggleButton_, &QToolButton::clicked, this,
          &MainWindow::toggleSelectedRouter);
  nodeLayout->addWidget(routerStateLabel_, 1);
  nodeLayout->addWidget(routerToggleButton_);
  controlLayout->addWidget(nodeBox);

  auto *linkBox = new QGroupBox(QStringLiteral("链路状态"), control);
  auto *linkLayout = new QVBoxLayout(linkBox);
  linkCombo_ = new QComboBox(linkBox);
  connect(linkCombo_, &QComboBox::currentIndexChanged, this,
          &MainWindow::selectedLinkChanged);
  auto *linkStateRow = new QHBoxLayout;
  linkStateLabel_ = new QLabel(QStringLiteral("—"), linkBox);
  linkToggleButton_ = new QToolButton(linkBox);
  linkToggleButton_->setText(QStringLiteral("切换"));
  connect(linkToggleButton_, &QToolButton::clicked, this,
          &MainWindow::toggleSelectedLink);
  linkStateRow->addWidget(linkStateLabel_, 1);
  linkStateRow->addWidget(linkToggleButton_);
  linkLayout->addWidget(linkCombo_);
  linkLayout->addLayout(linkStateRow);
  controlLayout->addWidget(linkBox);

  auto *prefixBox = new QGroupBox(QStringLiteral("前缀扰动"), control);
  auto *prefixLayout = new QVBoxLayout(prefixBox);
  prefixEdit_ = new QLineEdit(prefixBox);
  prefixEdit_->setPlaceholderText(QStringLiteral("例如 203.0.113.0/24"));
  auto *prefixButtons = new QHBoxLayout;
  auto *advertise = new QPushButton(QStringLiteral("发布"), prefixBox);
  auto *withdraw = new QPushButton(QStringLiteral("撤销"), prefixBox);
  connect(advertise, &QPushButton::clicked, this,
          &MainWindow::advertisePrefix);
  connect(withdraw, &QPushButton::clicked, this,
          &MainWindow::withdrawPrefix);
  prefixButtons->addWidget(advertise);
  prefixButtons->addWidget(withdraw);
  prefixLayout->addWidget(prefixEdit_);
  prefixLayout->addLayout(prefixButtons);
  controlLayout->addWidget(prefixBox);
  controlLayout->addStretch();
  tabs->addTab(control, QStringLiteral("运行控制"));

  layout->addWidget(tabs, 1);
  dock->setWidget(container);
  addDockWidget(Qt::RightDockWidgetArea, dock);
}

void MainWindow::buildEventDock() {
  auto *dock = new QDockWidget(QStringLiteral("BMP 事件"), this);
  dock->setObjectName(QStringLiteral("eventDock"));
  dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  auto *container = new QWidget(dock);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(5, 5, 5, 5);
  auto *filterRow = new QHBoxLayout;
  eventFilterEdit_ = new QLineEdit(container);
  eventFilterEdit_->setClearButtonEnabled(true);
  eventFilterEdit_->setPlaceholderText(
      QStringLiteral("过滤所有列：路由器、动作、ASN、前缀…"));
  followEventsCheck_ = new QCheckBox(QStringLiteral("跟随实时"), container);
  followEventsCheck_->setChecked(true);
  auto *clearButton = new QPushButton(QStringLiteral("清空表格"), container);
  auto *historyButton = new QPushButton(QStringLiteral("打开历史…"), container);
  connect(clearButton, &QPushButton::clicked, eventModel_,
          &EventTableModel::clear);
  connect(historyButton, &QPushButton::clicked, this, &MainWindow::openHistory);
  filterRow->addWidget(eventFilterEdit_, 1);
  filterRow->addWidget(followEventsCheck_);
  filterRow->addWidget(clearButton);
  filterRow->addWidget(historyButton);
  layout->addLayout(filterRow);

  eventProxy_ = new QSortFilterProxyModel(this);
  eventProxy_->setSourceModel(eventModel_);
  eventProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  eventProxy_->setFilterKeyColumn(-1);
  eventProxy_->setSortRole(Qt::DisplayRole);
  connect(eventFilterEdit_, &QLineEdit::textChanged, eventProxy_,
          &QSortFilterProxyModel::setFilterFixedString);
  eventView_ = new QTableView(container);
  eventView_->setModel(eventProxy_);
  eventView_->setAlternatingRowColors(true);
  eventView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  eventView_->setSelectionMode(QAbstractItemView::SingleSelection);
  eventView_->setSortingEnabled(true);
  eventView_->sortByColumn(EventTableModel::Id, Qt::AscendingOrder);
  eventView_->verticalHeader()->setVisible(false);
  eventView_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  eventView_->horizontalHeader()->setStretchLastSection(false);
  connect(eventView_, &QTableView::doubleClicked, this,
          &MainWindow::showEventDetails);
  layout->addWidget(eventView_, 1);
  dock->setWidget(container);
  addDockWidget(Qt::BottomDockWidgetArea, dock);
  resizeDocks({dock}, {280}, Qt::Vertical);
}

void MainWindow::connectEngine() {
  engine_ = new SimulationEngine;
  engine_->moveToThread(&engineThread_);
  connect(&engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
  connect(engine_, &SimulationEngine::runningChanged, this,
          &MainWindow::onRunningChanged);
  connect(engine_, &SimulationEngine::convergenceChanged, this,
          &MainWindow::onConvergenceChanged);
  connect(engine_, &SimulationEngine::statsChanged, this,
          &MainWindow::onStatsChanged);
  connect(engine_, &SimulationEngine::eventGenerated, eventStore_,
          &EventStore::appendEvent);
  connect(engine_, &SimulationEngine::routerSnapshotsChanged, this,
          &MainWindow::onRouterSnapshots);
  connect(engine_, &SimulationEngine::ribSnapshotReady, this,
          &MainWindow::onRibSnapshot);
  connect(engine_, &SimulationEngine::peersSnapshotReady, this,
          &MainWindow::onPeerSnapshots);
  connect(engine_, &SimulationEngine::bestPathChanged, this,
          &MainWindow::onBestPathChanged);
  connect(engine_, &SimulationEngine::routerStateChanged, this,
          &MainWindow::onRouterRuntimeState);
  connect(engine_, &SimulationEngine::linkStateChanged, this,
          &MainWindow::onLinkRuntimeState);
  connect(engine_, &SimulationEngine::errorOccurred, this,
          &MainWindow::onEngineError);
  engineThread_.setObjectName(QStringLiteral("BgpSimulationThread"));
  engineThread_.start();
}

void MainWindow::newTopology() {
  if (!maybeSave()) {
    return;
  }
  setTopology(starterTopology());
  setDirty(false);
}

void MainWindow::openTopology() {
  if (!maybeSave()) {
    return;
  }
  const auto path = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开 BGP 拓扑"),
      topologyPath_.isEmpty() ? QDir::currentPath()
                              : QFileInfo(topologyPath_).absolutePath(),
      QStringLiteral("Topology JSON (*.json);;所有文件 (*.*)"));
  if (!path.isEmpty()) {
    openTopologyPath(path);
  }
}

bool MainWindow::openTopologyPath(const QString &path, bool showErrors) {
  QString error;
  const auto topology = Topology::load(path, &error);
  if (!topology) {
    if (showErrors) {
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

bool MainWindow::saveTopology() {
  if (topologyPath_.isEmpty()) {
    return saveTopologyAs();
  }
  QString error;
  if (!topology_.save(topologyPath_, &error)) {
    QMessageBox::critical(this, QStringLiteral("保存失败"), error);
    return false;
  }
  setDirty(false);
  QSettings().setValue(QStringLiteral("files/lastTopology"), topologyPath_);
  statusBar()->showMessage(QStringLiteral("已保存 %1").arg(topologyPath_), 4000);
  return true;
}

bool MainWindow::saveTopologyAs() {
  auto suggested = topologyPath_;
  if (suggested.isEmpty()) {
    suggested = QDir::current().filePath(
        QStringLiteral("%1.json").arg(topology_.simulation.name));
  }
  auto path = QFileDialog::getSaveFileName(
      this, QStringLiteral("保存 BGP 拓扑"), suggested,
      QStringLiteral("Topology JSON (*.json)"));
  if (path.isEmpty()) {
    return false;
  }
  if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
    path += QStringLiteral(".json");
  }
  topologyPath_ = QFileInfo(path).absoluteFilePath();
  return saveTopology();
}

void MainWindow::openHistory() {
  const auto path = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开 BMP SQLite 日志"), QDir::currentPath(),
      QStringLiteral("SQLite database (*.sqlite *.db);;所有文件 (*.*)"));
  if (path.isEmpty()) {
    return;
  }
  QString error;
  auto events = EventStore::readDatabase(path, 50000, &error);
  if (!error.isEmpty()) {
    QMessageBox::critical(this, QStringLiteral("无法读取日志"), error);
    return;
  }
  eventModel_->setEvents(std::move(events));
  followEventsCheck_->setChecked(false);
  logPathLabel_->setText(
      QStringLiteral("历史：%1").arg(QDir::toNativeSeparators(path)));
  statusBar()->showMessage(QStringLiteral("已载入历史 BMP 日志"), 4000);
}

void MainWindow::editSimulationSettings() {
  SimulationSettingsDialog dialog(topology_.simulation, this);
  if (dialog.exec() == QDialog::Accepted) {
    topology_.simulation = dialog.settings();
    setDirty(true);
  }
}

void MainWindow::createRouter(const QPointF &position) {
  RouterConfig config;
  config.id = topology_.nextRouterName();
  config.routerId = topology_.nextBgpRouterId();
  config.clusterId = config.routerId;
  config.position = position;
  RouterDialog dialog(config, topology_.routers.keys(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const auto router = dialog.router();
  topology_.routers.insert(router.id, router);
  scene_->rebuild();
  refreshTopologySelectors();
  setDirty(true);
}

void MainWindow::editRouter(const QString &routerId) {
  const auto it = topology_.routers.constFind(routerId);
  if (it == topology_.routers.cend()) {
    return;
  }
  auto otherIds = topology_.routers.keys();
  otherIds.removeAll(routerId);
  RouterDialog dialog(it.value(), otherIds, this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const auto updated = dialog.router();
  if (updated.id != routerId) {
    topology_.routers.remove(routerId);
    for (auto &link : topology_.links) {
      if (link.a == routerId) {
        link.a = updated.id;
      }
      if (link.b == routerId) {
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

void MainWindow::createLink(const QString &a, const QString &b) {
  if (topology_.findLink(a, b)) {
    QMessageBox::information(this, QStringLiteral("链路已存在"),
                             QStringLiteral("%1 与 %2 已经连接。").arg(a, b));
    return;
  }
  LinkConfig config{.a = a, .b = b};
  LinkDialog dialog(config, this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  topology_.links.append(dialog.link());
  scene_->rebuild();
  refreshTopologySelectors();
  setDirty(true);
}

void MainWindow::editLink(const QString &a, const QString &b) {
  auto *link = topology_.findLink(a, b);
  if (!link) {
    return;
  }
  LinkDialog dialog(*link, this);
  if (dialog.exec() == QDialog::Accepted) {
    *link = dialog.link();
    scene_->rebuild();
    refreshTopologySelectors();
    setDirty(true);
  }
}

void MainWindow::deleteSelection() { scene_->deleteSelection(); }

void MainWindow::markDirty() {
  refreshTopologySelectors();
  setDirty(true);
}

void MainWindow::startSimulation() {
  const auto problems = topology_.validate();
  if (!problems.isEmpty()) {
    QMessageBox::critical(this, QStringLiteral("拓扑无效"), problems.join(u'\n'));
    return;
  }
  QString error;
  if (!eventStore_->beginRun(topology_.simulation, &error)) {
    QMessageBox::critical(this, QStringLiteral("无法创建日志"), error);
    return;
  }
  eventModel_->clear();
  followEventsCheck_->setChecked(true);
  bestPaths_.clear();
  runtimeRouters_.clear();
  runtimeLinks_.clear();
  scene_->clearRuntimeState();
  startAction_->setEnabled(false);
  const auto copy = topology_;
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, copy] { engine->startSimulation(copy); },
      Qt::QueuedConnection);
}

void MainWindow::stopSimulation() {
  stopAction_->setEnabled(false);
  QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation,
                            Qt::QueuedConnection);
}

void MainWindow::onRunningChanged(bool running) {
  simulationRunning_ = running;
  scene_->setEditable(!running);
  updateEditorActions();
  if (!running) {
    eventStore_->flush();
    simulationStatusLabel_->setText(QStringLiteral("● 已停止"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#6c757d"));
  } else {
    simulationStatusLabel_->setText(QStringLiteral("● 收敛中"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#d97706"));
  }
}

void MainWindow::onConvergenceChanged(bool converged) {
  simulationConverged_ = converged;
  if (simulationRunning_ && converged) {
    simulationStatusLabel_->setText(QStringLiteral("● 已收敛"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#16835d"));
  } else if (simulationRunning_) {
    simulationStatusLabel_->setText(QStringLiteral("● 收敛中"));
    simulationStatusLabel_->setStyleSheet(QStringLiteral("color:#d97706"));
  }
}

void MainWindow::onStatsChanged(const SimulationStats &stats) {
  statsLabel_->setText(QStringLiteral("消息 %1 · 待处理 %2 · %3 ms")
                           .arg(stats.deliveredMessages)
                           .arg(stats.pendingEvents)
                           .arg(stats.elapsedMs));
}

void MainWindow::onRouterSnapshots(QVector<RouterSnapshot> snapshots) {
  runtimeRouters_.clear();
  for (const auto &snapshot : snapshots) {
    runtimeRouters_.insert(snapshot.id, snapshot);
    scene_->setRouterRuntimeState(snapshot.id, snapshot.active);
  }
  refreshRuntimeControls();
  requestSelectedRouterSnapshot();
}

void MainWindow::onRibSnapshot(const RibSnapshot &snapshot) {
  if (snapshot.router != routerCombo_->currentText()) {
    return;
  }
  currentRib_ = snapshot;
  populateRibTables(snapshot);
}

void MainWindow::onPeerSnapshots(const QString &routerId,
                                 QVector<PeerSnapshot> snapshots) {
  if (routerId == routerCombo_->currentText()) {
    populatePeerTable(snapshots);
  }
}

void MainWindow::onBestPathChanged(const BestPathUpdate &update) {
  bestPaths_.insert(routeKey(update.router, update.prefix), update);
  if (update.router == routerCombo_->currentText()) {
    requestSelectedRouterSnapshot();
  }
}

void MainWindow::onRouterRuntimeState(const QString &routerId, bool enabled) {
  if (runtimeRouters_.contains(routerId)) {
    runtimeRouters_[routerId].active = enabled;
  }
  scene_->setRouterRuntimeState(routerId, enabled);
  refreshRuntimeControls();
}

void MainWindow::onLinkRuntimeState(const QString &a, const QString &b,
                                    bool enabled) {
  runtimeLinks_.insert(Topology::edgeKey(a, b), enabled);
  scene_->setLinkRuntimeState(a, b, enabled);
  refreshRuntimeControls();
}

void MainWindow::onEngineError(const QString &message) {
  QMessageBox::critical(this, QStringLiteral("仿真错误"), message);
}

void MainWindow::selectedRouterChanged() {
  requestSelectedRouterSnapshot();
  refreshRuntimeControls();
  scene_->highlightPath({});
}

void MainWindow::selectedLinkChanged() { refreshRuntimeControls(); }

void MainWindow::sceneSelectionChanged(const QString &routerId,
                                       const QString &linkA,
                                       const QString &linkB) {
  if (!routerId.isEmpty()) {
    routerCombo_->setCurrentText(routerId);
  }
  if (!linkA.isEmpty()) {
    const auto key = Topology::edgeKey(linkA, linkB);
    const auto index = linkCombo_->findData(key);
    if (index >= 0) {
      linkCombo_->setCurrentIndex(index);
    }
  }
}

void MainWindow::toggleSelectedRouter() {
  const auto id = routerCombo_->currentText();
  if (!simulationRunning_ || id.isEmpty() || !runtimeRouters_.contains(id)) {
    return;
  }
  const auto target = !runtimeRouters_.value(id).active;
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, id, target] {
        engine->setRouterState(id, target);
      },
      Qt::QueuedConnection);
}

void MainWindow::toggleSelectedLink() {
  if (!simulationRunning_ || linkCombo_->currentIndex() < 0) {
    return;
  }
  const auto index = linkCombo_->currentIndex();
  const auto a = linkCombo_->itemData(index, Qt::UserRole + 1).toString();
  const auto b = linkCombo_->itemData(index, Qt::UserRole + 2).toString();
  const auto key = Topology::edgeKey(a, b);
  const auto initial = topology_.findLink(a, b)
                           ? topology_.findLink(a, b)->enabled
                           : false;
  const auto target = !runtimeLinks_.value(key, initial);
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, a, b, target] {
        engine->setLinkState(a, b, target);
      },
      Qt::QueuedConnection);
}

void MainWindow::advertisePrefix() {
  const auto router = routerCombo_->currentText();
  const auto prefix = prefixEdit_->text().trimmed();
  if (!simulationRunning_ || router.isEmpty() || prefix.isEmpty()) {
    return;
  }
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, router, prefix] {
        engine->originatePrefix(router, prefix);
      },
      Qt::QueuedConnection);
}

void MainWindow::withdrawPrefix() {
  const auto router = routerCombo_->currentText();
  const auto prefix = prefixEdit_->text().trimmed();
  if (!simulationRunning_ || router.isEmpty() || prefix.isEmpty()) {
    return;
  }
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, router, prefix] {
        engine->withdrawPrefix(router, prefix);
      },
      Qt::QueuedConnection);
}

void MainWindow::highlightSelectedRoute() {
  const auto rows = ribTable_->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    scene_->highlightPath({});
    return;
  }
  const auto prefix = ribTable_->item(rows.front().row(), 0)->text();
  scene_->highlightPath(pathFor(routerCombo_->currentText(), prefix));
}

void MainWindow::showEventDetails(const QModelIndex &proxyIndex) {
  const auto sourceIndex = eventProxy_->mapToSource(proxyIndex);
  const auto *event = eventModel_->eventAt(sourceIndex.row());
  if (!event) {
    return;
  }
  auto *dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("事件 #%1").arg(event->id));
  dialog->resize(650, 480);
  auto *layout = new QVBoxLayout(dialog);
  auto *text = new QTextEdit(dialog);
  text->setReadOnly(true);
  text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  text->setPlainText(QString::fromUtf8(
      QJsonDocument(EventStore::eventToJson(*event))
          .toJson(QJsonDocument::Indented)));
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  layout->addWidget(text);
  layout->addWidget(buttons);
  dialog->show();
}

void MainWindow::setTopology(Topology topology, const QString &path) {
  topology_ = std::move(topology);
  topologyPath_ = path;
  bestPaths_.clear();
  runtimeRouters_.clear();
  runtimeLinks_.clear();
  currentRib_ = {};
  scene_->clearRuntimeState();
  scene_->setTopology(&topology_);
  refreshTopologySelectors();
  updateWindowTitle();
  QTimer::singleShot(0, this, [this] { fitAction_->trigger(); });
}

void MainWindow::setDirty(bool dirty) {
  dirty_ = dirty;
  saveAction_->setEnabled(dirty_ && !simulationRunning_);
  updateWindowTitle();
}

bool MainWindow::maybeSave() {
  if (!dirty_) {
    return true;
  }
  const auto choice = QMessageBox::warning(
      this, QStringLiteral("未保存的更改"),
      QStringLiteral("当前拓扑尚未保存。是否先保存？"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (choice == QMessageBox::Cancel) {
    return false;
  }
  return choice == QMessageBox::Discard || saveTopology();
}

void MainWindow::updateWindowTitle() {
  const auto file = topologyPath_.isEmpty()
                        ? QStringLiteral("未命名")
                        : QFileInfo(topologyPath_).fileName();
  setWindowTitle(QStringLiteral("%1%2 — BgpTester")
                     .arg(dirty_ ? QStringLiteral("*") : QString{}, file));
}

void MainWindow::updateEditorActions() {
  const auto editable = !simulationRunning_;
  for (auto *action : {newAction_, openAction_, saveAsAction_, settingsAction_,
                       selectModeAction_, addRouterAction_, addLinkAction_,
                       deleteAction_}) {
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

void MainWindow::refreshTopologySelectors() {
  const auto oldRouter = routerCombo_->currentText();
  routerCombo_->blockSignals(true);
  routerCombo_->clear();
  routerCombo_->addItems(topology_.routers.keys());
  const auto routerIndex = routerCombo_->findText(oldRouter);
  if (routerIndex >= 0) {
    routerCombo_->setCurrentIndex(routerIndex);
  }
  routerCombo_->blockSignals(false);

  const auto oldLink = linkCombo_->currentData().toString();
  linkCombo_->blockSignals(true);
  linkCombo_->clear();
  for (const auto &link : topology_.links) {
    const auto key = Topology::edgeKey(link.a, link.b);
    linkCombo_->addItem(QStringLiteral("%1 ↔ %2").arg(link.a, link.b), key);
    const auto row = linkCombo_->count() - 1;
    linkCombo_->setItemData(row, link.a, Qt::UserRole + 1);
    linkCombo_->setItemData(row, link.b, Qt::UserRole + 2);
  }
  const auto linkIndex = linkCombo_->findData(oldLink);
  if (linkIndex >= 0) {
    linkCombo_->setCurrentIndex(linkIndex);
  }
  linkCombo_->blockSignals(false);
  refreshRuntimeControls();
}

void MainWindow::requestSelectedRouterSnapshot() {
  const auto id = routerCombo_->currentText();
  if (!simulationRunning_ || id.isEmpty()) {
    currentRib_ = {};
    currentRib_.router = id;
    populateRibTables(currentRib_);
    populatePeerTable({});
    return;
  }
  QMetaObject::invokeMethod(
      engine_, [engine = engine_, id] { engine->requestRouterSnapshot(id); },
      Qt::QueuedConnection);
}

void MainWindow::populateRibTables(const RibSnapshot &snapshot) {
  ribTable_->setRowCount(snapshot.locRib.size());
  int row = 0;
  for (auto it = snapshot.locRib.cbegin(); it != snapshot.locRib.cend(); ++it) {
    const auto &route = it.value();
    ribTable_->setItem(row, 0, tableItem(route.prefix, Qt::AlignLeft));
    ribTable_->setItem(row, 1,
                       tableItem(route.localOrigin ? QStringLiteral("local")
                                                   : route.learnedFrom));
    ribTable_->setItem(row, 2, tableItem(toString(route.sourceSession)));
    ribTable_->setItem(row, 3, tableItem(route.attributes.nextHop));
    ribTable_->setItem(row, 4,
                       tableItem(asPathText(route.attributes.asPath),
                                 Qt::AlignLeft | Qt::AlignVCenter));
    ribTable_->setItem(
        row, 5, tableItem(QString::number(route.attributes.localPref)));
    ribTable_->setItem(row, 6,
                       tableItem(QString::number(route.attributes.med)));
    ++row;
  }

  struct PathRow {
    QString rib;
    RouteEntry route;
  };
  QVector<PathRow> paths;
  for (const auto &route : snapshot.localRoutes) {
    paths.append({QStringLiteral("Local"), route});
  }
  for (auto peer = snapshot.adjRibIn.cbegin(); peer != snapshot.adjRibIn.cend();
       ++peer) {
    for (const auto &route : peer.value()) {
      paths.append({QStringLiteral("Adj-In/%1").arg(peer.key()), route});
    }
  }
  std::sort(paths.begin(), paths.end(), [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.route.prefix, lhs.rib) <
           std::tie(rhs.route.prefix, rhs.rib);
  });
  allRoutesTable_->setRowCount(paths.size());
  for (int pathRow = 0; pathRow < paths.size(); ++pathRow) {
    const auto &entry = paths.at(pathRow);
    const auto best = snapshot.locRib.value(entry.route.prefix);
    const bool isBest = snapshot.locRib.contains(entry.route.prefix) &&
                        best == entry.route;
    allRoutesTable_->setItem(pathRow, 0,
                             tableItem(entry.route.prefix, Qt::AlignLeft));
    allRoutesTable_->setItem(pathRow, 1, tableItem(entry.rib));
    allRoutesTable_->setItem(
        pathRow, 2,
        tableItem(entry.route.localOrigin ? QStringLiteral("local")
                                          : entry.route.learnedFrom));
    allRoutesTable_->setItem(pathRow, 3,
                             tableItem(isBest ? QStringLiteral("✓") : QString{}));
    allRoutesTable_->setItem(pathRow, 4,
                             tableItem(entry.route.attributes.nextHop));
    allRoutesTable_->setItem(
        pathRow, 5,
        tableItem(asPathText(entry.route.attributes.asPath),
                  Qt::AlignLeft | Qt::AlignVCenter));
    allRoutesTable_->setItem(
        pathRow, 6,
        tableItem(QString::number(entry.route.attributes.localPref)));
    allRoutesTable_->setItem(
        pathRow, 7, tableItem(QString::number(entry.route.attributes.med)));
  }
}

void MainWindow::populatePeerTable(const QVector<PeerSnapshot> &snapshots) {
  peerTable_->setRowCount(snapshots.size());
  for (int row = 0; row < snapshots.size(); ++row) {
    const auto &peer = snapshots.at(row);
    peerTable_->setItem(row, 0, tableItem(peer.id));
    peerTable_->setItem(row, 1, tableItem(QString::number(peer.remoteAsn)));
    peerTable_->setItem(row, 2, tableItem(toString(peer.sessionType)));
    peerTable_->setItem(row, 3, tableItem(toString(peer.state)));
    peerTable_->setItem(row, 4,
                        tableItem(peer.rrClient ? QStringLiteral("是")
                                                : QStringLiteral("否")));
    peerTable_->setItem(row, 5,
                        tableItem(QStringLiteral("%1 ms").arg(peer.mraiMs)));
  }
}

void MainWindow::refreshRuntimeControls() {
  const auto router = routerCombo_->currentText();
  if (!simulationRunning_ || !runtimeRouters_.contains(router)) {
    routerStateLabel_->setText(QStringLiteral("—"));
    routerToggleButton_->setEnabled(false);
  } else {
    const auto active = runtimeRouters_.value(router).active;
    routerStateLabel_->setText(active ? QStringLiteral("运行中")
                                      : QStringLiteral("已关闭"));
    routerToggleButton_->setText(active ? QStringLiteral("关闭节点")
                                        : QStringLiteral("恢复节点"));
    routerToggleButton_->setEnabled(true);
  }

  if (!simulationRunning_ || linkCombo_->currentIndex() < 0) {
    linkStateLabel_->setText(QStringLiteral("—"));
    linkToggleButton_->setEnabled(false);
  } else {
    const auto index = linkCombo_->currentIndex();
    const auto a = linkCombo_->itemData(index, Qt::UserRole + 1).toString();
    const auto b = linkCombo_->itemData(index, Qt::UserRole + 2).toString();
    const auto key = Topology::edgeKey(a, b);
    const auto *link = topology_.findLink(a, b);
    const auto enabled = runtimeLinks_.value(key, link && link->enabled);
    linkStateLabel_->setText(enabled ? QStringLiteral("已连接")
                                     : QStringLiteral("已断开"));
    linkToggleButton_->setText(enabled ? QStringLiteral("断开链路")
                                       : QStringLiteral("恢复链路"));
    linkToggleButton_->setEnabled(true);
  }
}

QStringList MainWindow::pathFor(const QString &routerId,
                                const QString &prefix) const {
  QStringList path;
  QSet<QString> seen;
  auto current = routerId;
  while (!current.isEmpty() && !seen.contains(current)) {
    seen.insert(current);
    path.append(current);
    std::optional<RouteEntry> route;
    const auto cached = bestPaths_.constFind(routeKey(current, prefix));
    if (cached != bestPaths_.cend() && cached->valid) {
      route = cached->route;
    } else if (current == currentRib_.router &&
               currentRib_.locRib.contains(prefix)) {
      route = currentRib_.locRib.value(prefix);
    }
    if (!route || route->localOrigin || route->learnedFrom.isEmpty() ||
        route->learnedFrom == current) {
      break;
    }
    current = route->learnedFrom;
  }
  return path;
}

QString MainWindow::routeKey(const QString &routerId, const QString &prefix) {
  return routerId + u'\x1f' + prefix;
}

Topology MainWindow::starterTopology() {
  Topology topology;
  topology.simulation.name = QStringLiteral("quick-lab");
  RouterConfig r1{.id = QStringLiteral("R1"),
                  .routerId = QStringLiteral("10.0.0.1"),
                  .asn = 65001,
                  .clusterId = QStringLiteral("10.0.0.1"),
                  .originatedPrefixes = {QStringLiteral("10.1.0.0/24")},
                  .position = QPointF(180, 220)};
  RouterConfig r2{.id = QStringLiteral("R2"),
                  .routerId = QStringLiteral("10.0.0.2"),
                  .asn = 65002,
                  .clusterId = QStringLiteral("10.0.0.2"),
                  .originatedPrefixes = {QStringLiteral("10.2.0.0/24")},
                  .position = QPointF(450, 220)};
  topology.routers.insert(r1.id, r1);
  topology.routers.insert(r2.id, r2);
  topology.links.append(LinkConfig{.a = r1.id, .b = r2.id, .delayMs = 10});
  return topology;
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (closing_) {
    event->accept();
    return;
  }
  if (!maybeSave()) {
    event->ignore();
    return;
  }
  closing_ = true;
  QSettings settings;
  settings.setValue(QStringLiteral("main/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("main/state"), saveState());
  if (simulationRunning_) {
    QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation,
                              Qt::BlockingQueuedConnection);
  }
  eventStore_->endRun();
  event->accept();
}

} // namespace bgptester
