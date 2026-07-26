#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QThread>

#include <optional>

class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
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
class TopologyRouterListModel;
class TopologyLinkListModel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Starts an asynchronous load; true means the request was accepted.
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
    void editTopologyBatchProperties();
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
    void onStartupProgress(const QString& stage, qint64 completed, qint64 total);
    void onStartupCancelled();
    void onRouterSnapshots(QVector<bgptester::RouterSnapshot> snapshots);
    void onRibSnapshot(const bgptester::RibSnapshot& snapshot);
    void onPeerSnapshots(const QString& routerId, QVector<bgptester::PeerSnapshot> snapshots);
    void onRoutingStateChanged();
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
    void onEventFilterChanged(const QString& filter);
    void refreshEventFilter();
    void clearEventTable();

private:
    void buildActions();
    void buildMenusAndToolbar();
    void buildInspectorDock();
    void buildEventDock();
    void saveEventColumnVisibility() const;
    void updateEventCountLabel();
    void updateHistoryControls();
    void startHistoryFilterQuery(quint64 generation, const QString& path, const QString& filter);
    void startLiveCountQuery(quint64 requestId, quint64 runSerial, const QString& path, quint64 maxEventId,
                             const QString& filter);
    void trackQueryThread(QThread* thread);
    void stopAndWaitForQueryThreads();
    void stopTopologyLoad();
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
    void appendConvergenceEvents(const QVector<SimulationEvent>& events);
    void appendConvergenceRecord(quint64 sequence, const QString& triggerEvent, const QString& triggerContext, const QDateTime& completedAt,
                                 qint64 durationMs, qint64 confirmationDurationMs, std::optional<quint64> bgpMessageCount);
    void rebuildConvergenceHistory(const QVector<SimulationEvent>& events, qint64 totalCount);
    void clearConvergenceHistory(const QString& stateText);
    bool beginRuntimeMutation();
    void completeRuntimeMutation(bool engineConverged);
    void refreshRuntimeControls();
    void endEventRun(bool blocking = true);
    void flushEventStore();
    Topology topology_;
    QString topologyPath_;
    bool dirty_ = false;
    bool simulationRunning_ = false;
    bool simulationConverged_ = false;
    bool runtimeMutationPending_ = false;
    bool simulationStartPending_ = false;
    bool simulationStartCancelRequested_ = false;
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
    QTimer* eventFilterTimer_ = nullptr;
    QVector<SimulationEvent> pendingUiEvents_;
    quint64 eventRunSerial_ = 0;
    quint64 liveCountRequestId_ = 0;
    quint64 historyQueryGeneration_ = 0;
    qint64 eventTotalCount_ = 0;
    qint64 eventFilteredCount_ = 0;
    qint64 messageTotalCount_ = 0;
    qint64 messageFilteredCount_ = 0;
    bool eventFilteredCountKnown_ = true;
    bool eventCountQueryFailed_ = false;
    bool historyFilterQueryInProgress_ = false;
    QString historyDatabasePath_;
    QSet<QThread*> queryThreads_;
    QThread* historyLoadThread_ = nullptr;
    QThread* historyFilterThread_ = nullptr;
    QThread* liveCountQueryThread_ = nullptr;
    bool liveCountSnapshotPending_ = false;
    bool liveCountRefreshPending_ = false;
    bool liveCountDeltaActive_ = false;
    qint64 liveDeltaEventTotal_ = 0;
    qint64 liveDeltaEventFiltered_ = 0;
    qint64 liveDeltaMessageTotal_ = 0;
    qint64 liveDeltaMessageFiltered_ = 0;
    QThread* topologyLoadThread_ = nullptr;
    quint64 topologyLoadGeneration_ = 0;
    bool topologyLoadInProgress_ = false;

    QAction* newAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* openHistoryAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* batchTopologyAction_ = nullptr;
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
    TopologyRouterListModel* routerListModel_ = nullptr;
    TopologyLinkListModel* linkListModel_ = nullptr;
    QSortFilterProxyModel* linkFilterModel_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;
    QTableView* ribTable_ = nullptr;
    QTableView* allRoutesTable_ = nullptr;
    BestRoutesTableModel* bestRoutesModel_ = nullptr;
    AllRoutesTableModel* allRoutesModel_ = nullptr;
    QTableWidget* peerTable_ = nullptr;
    QLabel* routerStateLabel_ = nullptr;
    QToolButton* routerToggleButton_ = nullptr;
    QLabel* linkStateLabel_ = nullptr;
    QLineEdit* linkSearchEdit_ = nullptr;
    QToolButton* linkBrowseButton_ = nullptr;
    QToolButton* linkToggleButton_ = nullptr;
    QGroupBox* prefixControlBox_ = nullptr;
    QLineEdit* prefixEdit_ = nullptr;
    QTableView* eventView_ = nullptr;
    QLineEdit* eventFilterEdit_ = nullptr;
    QCheckBox* followEventsCheck_ = nullptr;
    QPushButton* historyButton_ = nullptr;
    QLabel* eventCountLabel_ = nullptr;
    QTableWidget* convergenceTable_ = nullptr;
    QLabel* convergenceStateLabel_ = nullptr;
    QLabel* convergenceCountLabel_ = nullptr;
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
