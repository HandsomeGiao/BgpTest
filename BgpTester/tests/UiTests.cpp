#include "model/Topology.hpp"
#include "ui/Dialogs.hpp"
#include "ui/TopologyScene.hpp"

#include <QApplication>
#include <QComboBox>
#include <QGraphicsItem>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

#include <iostream>
#include <stdexcept>

namespace
{

using namespace bgptester;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

Topology draggableTopology()
{
    Topology topology;
    const RouterConfig r1{.id = QStringLiteral("R1"),
                          .routerId = QStringLiteral("10.0.0.1"),
                          .asn = 65000,
                          .clusterId = QStringLiteral("10.0.0.1"),
                          .originatedPrefixes = {},
                          .position = QPointF(250.0, 250.0),
                          .pluginId = StandardRouterPluginId,
                          .pluginSettings = {}};
    const RouterConfig r2{.id = QStringLiteral("R2"),
                          .routerId = QStringLiteral("10.0.0.2"),
                          .asn = 65000,
                          .clusterId = QStringLiteral("10.0.0.2"),
                          .originatedPrefixes = {},
                          .position = QPointF(500.0, 250.0),
                          .pluginId = StandardRouterPluginId,
                          .pluginSettings = {}};
    const RouterConfig r3{.id = QStringLiteral("R3"),
                          .routerId = QStringLiteral("10.0.0.3"),
                          .asn = 65001,
                          .clusterId = QStringLiteral("10.0.0.3"),
                          .originatedPrefixes = {},
                          .position = QPointF(375.0, 450.0),
                          .pluginId = StandardRouterPluginId,
                          .pluginSettings = {}};
    topology.routers.insert(r1.id, r1);
    topology.routers.insert(r2.id, r2);
    topology.routers.insert(r3.id, r3);
    topology.links.append(LinkConfig{.a = r1.id, .b = r2.id, .delayMs = 10});
    topology.links.append(LinkConfig{.a = r1.id, .b = r3.id, .delayMs = 20});
    return topology;
}

QGraphicsItem* routerItemAt(const TopologyScene& scene, const QPointF& position)
{
    for (auto* item : scene.items())
    {
        if (item->type() == QGraphicsItem::UserType + 1 && item->pos() == position)
        {
            return item;
        }
    }
    return nullptr;
}

void routerDragFinishesWithoutReentrantSceneResize(QApplication& application)
{
    auto topology = draggableTopology();
    const auto initialPosition = topology.routers.value(QStringLiteral("R1")).position;

    TopologyScene scene;
    TopologyView view(&scene);
    view.resize(900, 700);
    scene.setTopology(&topology);
    view.show();
    application.processEvents();

    auto* router = routerItemAt(scene, initialPosition);
    require(router, "could not find R1 graphics item");
    view.centerOn(router);
    application.processEvents();

    QSignalSpy modifiedSpy(&scene, &TopologyScene::topologyModified);
    const auto start = view.mapFromScene(router->scenePos());
    require(view.viewport()->rect().contains(start), "R1 is outside the test viewport");

    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
    for (int offset = 1; offset <= 180; ++offset)
    {
        QTest::mouseMove(view.viewport(), start + QPoint(offset, offset / 3), 1);
    }
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, start + QPoint(180, 60));
    application.processEvents();

    require(router->pos() != initialPosition, "drag did not move R1");
    require(topology.routers.value(QStringLiteral("R1")).position == router->pos(), "dragged position was not saved to topology");
    require(modifiedSpy.count() == 1, "one drag must emit topologyModified exactly once");
    require(scene.sceneRect().contains(router->sceneBoundingRect()), "scene rectangle was not updated after drag finished");

    const auto stationaryPoint = view.mapFromScene(router->scenePos());
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, stationaryPoint);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, stationaryPoint);
    application.processEvents();
    require(modifiedSpy.count() == 1, "clicking without moving must not mark topology modified again");
}

void selectedRouterIdsTrackMultiSelection()
{
    auto topology = draggableTopology();
    TopologyScene scene;
    scene.setTopology(&topology);

    auto* r1 = routerItemAt(scene, topology.routers.value(QStringLiteral("R1")).position);
    auto* r3 = routerItemAt(scene, topology.routers.value(QStringLiteral("R3")).position);
    require(r1 && r3, "could not find router items for selection test");
    require(scene.selectedRouterIds().isEmpty(), "a new scene unexpectedly contains selected routers");

    r3->setSelected(true);
    r1->setSelected(true);
    const QStringList expected{QStringLiteral("R1"), QStringLiteral("R3")};
    require(scene.selectedRouterIds() == expected, "selected router IDs do not reflect the multi-selection");

    for (auto* item : scene.items())
    {
        if (item->type() == QGraphicsItem::UserType + 2)
        {
            item->setSelected(true);
            break;
        }
    }
    require(scene.selectedRouterIds() == expected, "selected links must not be returned as selected routers");
}

void batchDialogOffersRouterKindsWithoutChangingAnythingByDefault()
{
    const auto topology = draggableTopology();
    QVector<RouterConfig> routers{topology.routers.value(QStringLiteral("R1")), topology.routers.value(QStringLiteral("R2"))};
    TopologyBatchEditDialog dialog(topology.links, routers, TopologyBatchEditDialog::RouterScope::Selection);

    auto* pluginCombo = dialog.findChild<QComboBox*>(QStringLiteral("batchRouterPluginCombo"));
    auto* summaryLabel = dialog.findChild<QLabel*>(QStringLiteral("batchTopologySummaryLabel"));
    require(pluginCombo, "batch router kind combo was not created");
    require(summaryLabel && summaryLabel->text().contains(QStringLiteral("选中的 2 台路由器")),
            "batch dialog does not describe the selected-router scope");
    require(dialog.routerPluginId().isEmpty(), "batch dialog must preserve router kinds until a kind is selected");
    require(dialog.delayMode() == TopologyBatchEditDialog::DelayMode::Unchanged,
            "batch dialog must preserve link delays until a delay mode is selected");

    const auto standardIndex = pluginCombo->findData(StandardRouterPluginId);
    require(standardIndex >= 0, "standard BGP router kind is missing from the batch dialog");
    pluginCombo->setCurrentIndex(standardIndex);
    require(dialog.routerPluginId() == StandardRouterPluginId, "batch dialog did not return the selected router kind");
    require(dialog.routerPluginDefaultSettings().isEmpty(), "standard BGP router defaults unexpectedly contain settings");
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try
    {
        routerDragFinishesWithoutReentrantSceneResize(application);
        selectedRouterIdsTrackMultiSelection();
        batchDialogOffersRouterKindsWithoutChangingAnythingByDefault();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All UI tests passed\n";
    return 0;
}
