from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import QLineF, QPointF, Qt
from PyQt6.QtGui import QAction, QBrush, QColor, QPen
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFormLayout,
    QGraphicsEllipseItem,
    QGraphicsItem,
    QGraphicsLineItem,
    QGraphicsScene,
    QGraphicsSimpleTextItem,
    QGraphicsView,
    QHBoxLayout,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QToolBar,
    QVBoxLayout,
)

from .models import LinkEdge, RouterNode, TopologyModel


class RouterDialog(QDialog):
    def __init__(self, router: Optional[RouterNode] = None, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Router")
        self.id_edit = QLineEdit(router.id if router else "")
        self.router_id_edit = QLineEdit(router.router_id if router else "")
        self.asn_spin = QSpinBox()
        self.asn_spin.setRange(1, 10_000_000)
        self.asn_spin.setValue(router.asn if router else 65000)
        self.rr_check = QCheckBox()
        self.rr_check.setChecked(router.route_reflector if router else False)
        self.cluster_edit = QLineEdit(router.cluster_id if router else "")
        self.prefixes_edit = QPlainTextEdit("\n".join(router.originated_prefixes) if router else "")

        form = QFormLayout()
        form.addRow("Node id", self.id_edit)
        form.addRow("BGP router id", self.router_id_edit)
        form.addRow("ASN", self.asn_spin)
        form.addRow("Route reflector", self.rr_check)
        form.addRow("Cluster id", self.cluster_edit)
        form.addRow("Originated prefixes", self.prefixes_edit)

        ok = QPushButton("OK")
        cancel = QPushButton("Cancel")
        ok.clicked.connect(self.accept)
        cancel.clicked.connect(self.reject)
        buttons = QHBoxLayout()
        buttons.addStretch()
        buttons.addWidget(ok)
        buttons.addWidget(cancel)

        layout = QVBoxLayout()
        layout.addLayout(form)
        layout.addLayout(buttons)
        self.setLayout(layout)

    def router(self, x: float = 100.0, y: float = 100.0) -> RouterNode:
        prefixes = [
            line.strip()
            for line in self.prefixes_edit.toPlainText().splitlines()
            if line.strip()
        ]
        router_id = self.router_id_edit.text().strip()
        return RouterNode(
            id=self.id_edit.text().strip(),
            router_id=router_id,
            asn=self.asn_spin.value(),
            route_reflector=self.rr_check.isChecked(),
            cluster_id=self.cluster_edit.text().strip() or router_id,
            originated_prefixes=prefixes,
            x=x,
            y=y,
        )


class LinkDialog(QDialog):
    def __init__(
        self,
        router_ids: list[str],
        link: Optional[LinkEdge] = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Link")
        self.a_combo = QComboBox()
        self.b_combo = QComboBox()
        self.a_combo.addItems(router_ids)
        self.b_combo.addItems(router_ids)
        self.enabled_check = QCheckBox()
        self.enabled_check.setChecked(True)
        self.delay_spin = QSpinBox()
        self.delay_spin.setRange(0, 60_000)
        self.delay_spin.setValue(1)
        self.rr_a_check = QCheckBox("A treats B as RR client")
        self.rr_b_check = QCheckBox("B treats A as RR client")

        if link:
            self.a_combo.setCurrentText(link.a)
            self.b_combo.setCurrentText(link.b)
            self.enabled_check.setChecked(link.enabled)
            self.delay_spin.setValue(link.delay_ms)
            self.rr_a_check.setChecked(link.rr_client_from_a)
            self.rr_b_check.setChecked(link.rr_client_from_b)

        form = QFormLayout()
        form.addRow("A", self.a_combo)
        form.addRow("B", self.b_combo)
        form.addRow("Enabled", self.enabled_check)
        form.addRow("Delay ms", self.delay_spin)
        form.addRow("", self.rr_a_check)
        form.addRow("", self.rr_b_check)

        ok = QPushButton("OK")
        cancel = QPushButton("Cancel")
        ok.clicked.connect(self.accept)
        cancel.clicked.connect(self.reject)
        buttons = QHBoxLayout()
        buttons.addStretch()
        buttons.addWidget(ok)
        buttons.addWidget(cancel)

        layout = QVBoxLayout()
        layout.addLayout(form)
        layout.addLayout(buttons)
        self.setLayout(layout)

    def link(self) -> LinkEdge:
        return LinkEdge(
            a=self.a_combo.currentText(),
            b=self.b_combo.currentText(),
            enabled=self.enabled_check.isChecked(),
            delay_ms=self.delay_spin.value(),
            rr_client_from_a=self.rr_a_check.isChecked(),
            rr_client_from_b=self.rr_b_check.isChecked(),
        )


class RouterItem(QGraphicsEllipseItem):
    def __init__(self, router: RouterNode, scene_ref: "TopologyScene") -> None:
        super().__init__(-34, -24, 68, 48)
        self.router = router
        self.scene_ref = scene_ref
        self.setPos(QPointF(router.x, router.y))
        self.setBrush(QBrush(QColor("#d8f0ff") if router.route_reflector else QColor("#f7f7f7")))
        self.setPen(QPen(QColor("#25607a"), 2))
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)

        label = QGraphicsSimpleTextItem(router.id, self)
        label_rect = label.boundingRect()
        label.setPos(-label_rect.width() / 2, -label_rect.height() / 2)

    def itemChange(self, change: QGraphicsItem.GraphicsItemChange, value):
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged:
            point = self.pos()
            self.router.x = point.x()
            self.router.y = point.y()
            self.scene_ref.update_links()
        return super().itemChange(change, value)


class LinkItem(QGraphicsLineItem):
    def __init__(self, link: LinkEdge) -> None:
        super().__init__()
        self.link = link
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable)
        self.update_pen()

    def update_pen(self) -> None:
        color = QColor("#444444") if self.link.enabled else QColor("#b0b0b0")
        pen = QPen(color, 2, Qt.PenStyle.SolidLine if self.link.enabled else Qt.PenStyle.DashLine)
        self.setPen(pen)


class TopologyScene(QGraphicsScene):
    def __init__(self, model: TopologyModel) -> None:
        super().__init__()
        self.model = model
        self.router_items: dict[str, RouterItem] = {}
        self.link_items: list[LinkItem] = []
        self.setSceneRect(-2000, -2000, 4000, 4000)
        self.rebuild()

    def rebuild(self) -> None:
        self.clear()
        self.router_items.clear()
        self.link_items.clear()
        for link in self.model.links:
            item = LinkItem(link)
            item.setZValue(-1)
            self.link_items.append(item)
            self.addItem(item)
        for router in self.model.routers.values():
            item = RouterItem(router, self)
            self.router_items[router.id] = item
            self.addItem(item)
        self.update_links()

    def update_links(self) -> None:
        for item in self.link_items:
            a_item = self.router_items.get(item.link.a)
            b_item = self.router_items.get(item.link.b)
            if not a_item or not b_item:
                continue
            item.setLine(QLineF(a_item.pos(), b_item.pos()))
            item.update_pen()


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("TopoGenerator")
        self.resize(1100, 760)
        self.model = TopologyModel()
        self.scene = TopologyScene(self.model)
        self.view = QGraphicsView(self.scene)
        self.view.setRenderHints(self.view.renderHints())
        self.setCentralWidget(self.view)
        self._build_toolbar()

    def _build_toolbar(self) -> None:
        toolbar = QToolBar("Topology")
        self.addToolBar(toolbar)
        actions = [
            ("Add Router", self.add_router),
            ("Add Link", self.add_link),
            ("Edit", self.edit_selected),
            ("Delete", self.delete_selected),
            ("Load", self.load_json),
            ("Export", self.export_json),
        ]
        for text, callback in actions:
            action = QAction(text, self)
            action.triggered.connect(callback)
            toolbar.addAction(action)

    def add_router(self) -> None:
        index = len(self.model.routers) + 1
        default_router = RouterNode(
            id=f"R{index}",
            router_id=f"{index}.{index}.{index}.{index}",
            asn=65000,
            x=120 + index * 50,
            y=120 + index * 35,
        )
        dialog = RouterDialog(default_router, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            router = dialog.router(default_router.x, default_router.y)
            if not router.id:
                self._error("Router id is required")
                return
            self.model.add_router(router)
            self.scene.rebuild()

    def add_link(self) -> None:
        if len(self.model.routers) < 2:
            self._error("Create at least two routers first")
            return
        dialog = LinkDialog(sorted(self.model.routers.keys()), parent=self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            try:
                self.model.add_link(dialog.link())
                self.scene.rebuild()
            except ValueError as exc:
                self._error(str(exc))

    def edit_selected(self) -> None:
        selected = self.scene.selectedItems()
        if not selected:
            return
        item = selected[0]
        if isinstance(item, RouterItem):
            dialog = RouterDialog(item.router, self)
            if dialog.exec() == QDialog.DialogCode.Accepted:
                old_id = item.router.id
                updated = dialog.router(item.router.x, item.router.y)
                if not updated.id:
                    self._error("Router id is required")
                    return
                for link in self.model.links:
                    if link.a == old_id:
                        link.a = updated.id
                    if link.b == old_id:
                        link.b = updated.id
                self.model.routers.pop(old_id, None)
                self.model.add_router(updated)
                self.scene.rebuild()
        elif isinstance(item, LinkItem):
            dialog = LinkDialog(sorted(self.model.routers.keys()), item.link, self)
            if dialog.exec() == QDialog.DialogCode.Accepted:
                updated = dialog.link()
                item.link.a = updated.a
                item.link.b = updated.b
                item.link.enabled = updated.enabled
                item.link.delay_ms = updated.delay_ms
                item.link.rr_client_from_a = updated.rr_client_from_a
                item.link.rr_client_from_b = updated.rr_client_from_b
                self.scene.rebuild()

    def delete_selected(self) -> None:
        selected = self.scene.selectedItems()
        if not selected:
            return
        item = selected[0]
        if isinstance(item, RouterItem):
            self.model.remove_router(item.router.id)
        elif isinstance(item, LinkItem):
            self.model.remove_link(item.link.a, item.link.b)
        self.scene.rebuild()

    def load_json(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load topology", "", "JSON (*.json)")
        if not path:
            return
        try:
            with Path(path).open("r", encoding="utf-8") as handle:
                self.model = TopologyModel.from_json(json.load(handle))
            self.scene = TopologyScene(self.model)
            self.view.setScene(self.scene)
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
            self._error(f"Failed to load topology: {exc}")

    def export_json(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Export topology", "topology.json", "JSON (*.json)")
        if not path:
            return
        try:
            with Path(path).open("w", encoding="utf-8") as handle:
                json.dump(self.model.to_json(), handle, indent=2)
                handle.write("\n")
        except OSError as exc:
            self._error(f"Failed to export topology: {exc}")

    def _error(self, message: str) -> None:
        QMessageBox.critical(self, "TopoGenerator", message)


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
