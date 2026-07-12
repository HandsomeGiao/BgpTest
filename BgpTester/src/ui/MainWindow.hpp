#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QMainWindow>
#include <QMap>
#include <QThread>

class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSortFilterProxyModel;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTimer;
class QToolButton;

namespace bgptester
{

class EventStore;
class EventTableModel;
class AllRoutesTableModel;
class BestRoutesTableModel;
class SimulationEngine;
class TopologyScene;
class TopologyView;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openTopologyPath(const QString& path, bool showErrors = true);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void newTopology();
    void openTopology();
    bool saveTopology();
    bool saveTopologyAs();
    void openHistory();
    void editSimulationSettings();
    void createRouter(const QPointF& position);
    void editRouter(const QString& routerId);
    void createLink(const QString& a, const QString& b);
    void editLink(const QString& a, const QString& b);
    void deleteSelection();
    void markDirty();

    void startSimulation();
    void stopSimulation();
    void onRunningChanged(bool running);
    void onConvergenceChanged(bool converged);
    void onStatsChanged(const bgptester::SimulationStats& stats);
    void onRouterSnapshots(QVector<bgptester::RouterSnapshot> snapshots);
    void onRibSnapshot(const bgptester::RibSnapshot& snapshot);
    void onPeerSnapshots(const QString& routerId, QVector<bgptester::PeerSnapshot> snapshots);
    void onRibChanged(const QString& routerId);
    void onPathReady(const QString& routerId, const QString& prefix, const QStringList& path);
    void onRouterRuntimeState(const QString& routerId, bool enabled);
    void onLinkRuntimeState(const QString& a, const QString& b, bool enabled);
    void onEngineError(const QString& message);

    void selectedRouterChanged();
    void selectedLinkChanged();
    void sceneSelectionChanged(const QString& routerId, const QString& linkA, const QString& linkB);
    void toggleSelectedRouter();
    void toggleSelectedLink();
    void advertisePrefix();
    void withdrawPrefix();
    void highlightSelectedRoute();
    void showEventDetails(const QModelIndex& proxyIndex);
    void enqueueStoredEvents(quint64 runSerial, QVector<bgptester::SimulationEvent> events);
    void drainUiEventQueue();

private:
    void buildActions();
    void buildMenusAndToolbar();
    void buildInspectorDock();
    void buildEventDock();
    void connectEngine();
    void setTopology(Topology topology, const QString& path = {});
    void setDirty(bool dirty);
    bool maybeSave();
    void updateWindowTitle();
    void updateEditorActions();
    void refreshTopologySelectors();
    void requestSelectedRouterSnapshot();
    void scheduleSelectedRouterSnapshot();
    void populateRibTables(const RibSnapshot& snapshot);
    void populatePeerTable(const QVector<PeerSnapshot>& snapshots);
    void refreshRuntimeControls();
    bool beginEventRun(QString* error);
    void endEventRun(bool blocking = true);
    void flushEventStore();
    static Topology starterTopology();

    Topology topology_;
    QString topologyPath_;
    bool dirty_ = false;
    bool simulationRunning_ = false;
    bool simulationConverged_ = false;
    bool simulationStartPending_ = false;
    bool closing_ = false;

    TopologyScene* scene_ = nullptr;
    TopologyView* view_ = nullptr;
    SimulationEngine* engine_ = nullptr;
    QThread engineThread_;
    EventStore* eventStore_ = nullptr;
    QThread eventStoreThread_;
    EventTableModel* eventModel_ = nullptr;
    QSortFilterProxyModel* eventProxy_ = nullptr;
    QTimer* uiEventDrainTimer_ = nullptr;
    QVector<SimulationEvent> pendingUiEvents_;
    quint64 eventRunSerial_ = 0;

    QAction* newAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* openHistoryAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* selectModeAction_ = nullptr;
    QAction* addRouterAction_ = nullptr;
    QAction* addLinkAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* startAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* fitAction_ = nullptr;
    QActionGroup* modeGroup_ = nullptr;
    QMenu* viewMenu_ = nullptr;

    QComboBox* routerCombo_ = nullptr;
    QComboBox* linkCombo_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;
    QTableView* ribTable_ = nullptr;
    QTableView* allRoutesTable_ = nullptr;
    BestRoutesTableModel* bestRoutesModel_ = nullptr;
    AllRoutesTableModel* allRoutesModel_ = nullptr;
    QTableWidget* peerTable_ = nullptr;
    QLabel* routerStateLabel_ = nullptr;
    QToolButton* routerToggleButton_ = nullptr;
    QLabel* linkStateLabel_ = nullptr;
    QToolButton* linkToggleButton_ = nullptr;
    QLineEdit* prefixEdit_ = nullptr;
    QTableView* eventView_ = nullptr;
    QLineEdit* eventFilterEdit_ = nullptr;
    QCheckBox* followEventsCheck_ = nullptr;
    QLabel* eventCountLabel_ = nullptr;
    QLabel* simulationStatusLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QLabel* logPathLabel_ = nullptr;

    RibSnapshot currentRib_;
    QMap<QString, RouterSnapshot> runtimeRouters_;
    QMap<QString, bool> runtimeLinks_;
    QTimer* ribRefreshTimer_ = nullptr;
    bool snapshotRequestPending_ = false;
    bool snapshotRefreshNeeded_ = false;
    bool historyLoadInProgress_ = false;
};

} // namespace bgptester
