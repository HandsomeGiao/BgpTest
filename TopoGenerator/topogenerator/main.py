from __future__ import annotations

import argparse
import json
import math
import random
import re
import sys
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import QObject, QLineF, QPoint, QPointF, QRectF, QSettings, Qt, QTimer
from PyQt6.QtGui import QAction, QBrush, QColor, QPainter, QPen, QPolygonF
from PyQt6.QtNetwork import QLocalSocket
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QCompleter,
    QDialog,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGraphicsEllipseItem,
    QGraphicsItem,
    QGraphicsLineItem,
    QGraphicsRectItem,
    QGraphicsScene,
    QGraphicsSimpleTextItem,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

try:
    from .models import LinkEdge, RouterNode, TopologyModel, router_id_from_index
except ImportError:
    from models import LinkEdge, RouterNode, TopologyModel, router_id_from_index


AS_COLORS = [
    "#2f80ed",
    "#27ae60",
    "#eb5757",
    "#9b51e0",
    "#f2994a",
    "#00a3a3",
    "#d9468a",
    "#6f7d00",
]

DEFAULT_OBSERVER_PIPE = "TopoSimulatorObserver"


def color_for_asn(asn: int) -> QColor:
    return QColor(AS_COLORS[abs(asn) % len(AS_COLORS)])


def natural_sort_key(text: str) -> tuple[tuple[int, object], ...]:
    return tuple(
        (0, int(part)) if part.isdigit() else (1, part.casefold())
        for part in re.split(r"(\d+)", text)
        if part
    )


class RouterDialog(QDialog):
    def __init__(self, router: Optional[RouterNode] = None, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Router")
        self.id_edit = QLineEdit(router.id if router else "")
        self.router_id_edit = QLineEdit(router.router_id if router else "")
        self.asn_spin = QSpinBox()
        self.asn_spin.setRange(1, 10_000_000)
        self.asn_spin.setValue(router.asn if router else 65000)
        self.cluster_edit = QLineEdit(router.cluster_id if router else "")
        self.prefixes_edit = QPlainTextEdit("\n".join(router.originated_prefixes) if router else "")

        form = QFormLayout()
        form.addRow("Node id", self.id_edit)
        form.addRow("BGP router id", self.router_id_edit)
        form.addRow("ASN", self.asn_spin)
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
            cluster_id=self.cluster_edit.text().strip() or router_id,
            originated_prefixes=prefixes,
            x=x,
            y=y,
        )


class BatchRouterAsnDialog(QDialog):
    def __init__(self, routers: list[RouterNode], parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit Routers")
        self.asn_spin = QSpinBox()
        self.asn_spin.setRange(1, 10_000_000)
        self.asn_spin.setValue(routers[0].asn if routers else 65000)

        form = QFormLayout()
        form.addRow(f"ASN for {len(routers)} routers", self.asn_spin)

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

    def asn(self) -> int:
        return self.asn_spin.value()


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
        self.delay_spin.setValue(0)
        self.mrai_a_spin = QSpinBox()
        self.mrai_a_spin.setRange(0, 3_600_000)
        self.mrai_a_spin.setValue(0)
        self.mrai_b_spin = QSpinBox()
        self.mrai_b_spin.setRange(0, 3_600_000)
        self.mrai_b_spin.setValue(0)
        self.rr_a_check = QCheckBox("A treats B as RR client")
        self.rr_b_check = QCheckBox("B treats A as RR client")

        if link:
            self.a_combo.setCurrentText(link.a)
            self.b_combo.setCurrentText(link.b)
            self.enabled_check.setChecked(link.enabled)
            self.delay_spin.setValue(link.delay_ms)
            self.mrai_a_spin.setValue(link.mrai_ms_from_a)
            self.mrai_b_spin.setValue(link.mrai_ms_from_b)
            self.rr_a_check.setChecked(link.rr_client_from_a)
            self.rr_b_check.setChecked(link.rr_client_from_b)

        form = QFormLayout()
        form.addRow("A", self.a_combo)
        form.addRow("B", self.b_combo)
        form.addRow("Enabled", self.enabled_check)
        form.addRow("Delay ms", self.delay_spin)
        form.addRow("A -> B MRAI ms", self.mrai_a_spin)
        form.addRow("B -> A MRAI ms", self.mrai_b_spin)
        form.addRow("RR client", self.rr_a_check)
        form.addRow("RR client", self.rr_b_check)

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
            mrai_ms_from_a=self.mrai_a_spin.value(),
            mrai_ms_from_b=self.mrai_b_spin.value(),
        )


class BatchUpdateAllDialog(QDialog):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("BatchUpdateAll")

        self.delay_mode = QComboBox()
        self.delay_mode.addItems(["Fixed value", "Random closed range"])
        self.delay_value_spin = QSpinBox()
        self.delay_value_spin.setRange(0, 60_000)
        self.delay_min_spin = QSpinBox()
        self.delay_min_spin.setRange(0, 60_000)
        self.delay_max_spin = QSpinBox()
        self.delay_max_spin.setRange(0, 60_000)

        self.mrai_mode = QComboBox()
        self.mrai_mode.addItems(["Fixed value", "Random closed range"])
        self.mrai_value_spin = QSpinBox()
        self.mrai_value_spin.setRange(0, 3_600_000)
        self.mrai_min_spin = QSpinBox()
        self.mrai_min_spin.setRange(0, 3_600_000)
        self.mrai_max_spin = QSpinBox()
        self.mrai_max_spin.setRange(0, 3_600_000)

        self.delay_mode.currentIndexChanged.connect(self._update_enabled_state)
        self.mrai_mode.currentIndexChanged.connect(self._update_enabled_state)
        self._update_enabled_state()

        form = QFormLayout()
        form.addRow("Delay mode", self.delay_mode)
        form.addRow("Delay value ms", self.delay_value_spin)
        form.addRow("Delay min ms", self.delay_min_spin)
        form.addRow("Delay max ms", self.delay_max_spin)
        form.addRow("MRAI mode", self.mrai_mode)
        form.addRow("MRAI value ms", self.mrai_value_spin)
        form.addRow("MRAI min ms", self.mrai_min_spin)
        form.addRow("MRAI max ms", self.mrai_max_spin)

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

    def _update_enabled_state(self, *_args) -> None:
        delay_random = self.delay_mode.currentIndex() == 1
        self.delay_value_spin.setEnabled(not delay_random)
        self.delay_min_spin.setEnabled(delay_random)
        self.delay_max_spin.setEnabled(delay_random)

        mrai_random = self.mrai_mode.currentIndex() == 1
        self.mrai_value_spin.setEnabled(not mrai_random)
        self.mrai_min_spin.setEnabled(mrai_random)
        self.mrai_max_spin.setEnabled(mrai_random)

    def accept(self) -> None:
        if (
            self.delay_mode.currentIndex() == 1
            and self.delay_min_spin.value() > self.delay_max_spin.value()
        ):
            QMessageBox.warning(self, "BatchUpdateAll", "Delay min must be <= max.")
            return
        if (
            self.mrai_mode.currentIndex() == 1
            and self.mrai_min_spin.value() > self.mrai_max_spin.value()
        ):
            QMessageBox.warning(self, "BatchUpdateAll", "MRAI min must be <= max.")
            return
        super().accept()

    def delay_ms(self) -> int:
        if self.delay_mode.currentIndex() == 0:
            return self.delay_value_spin.value()
        return random.randint(self.delay_min_spin.value(), self.delay_max_spin.value())

    def mrai_ms(self) -> int:
        if self.mrai_mode.currentIndex() == 0:
            return self.mrai_value_spin.value()
        return random.randint(self.mrai_min_spin.value(), self.mrai_max_spin.value())


class RouterItem(QGraphicsEllipseItem):
    def __init__(self, router: RouterNode, scene_ref: "TopologyScene") -> None:
        super().__init__(-34, -24, 68, 48)
        self.router = router
        self.scene_ref = scene_ref
        self.setPos(QPointF(router.x, router.y))
        self.setBrush(QBrush(QColor("#d8f0ff") if scene_ref.is_route_reflector(router.id) else QColor("#f7f7f7")))
        self.setPen(QPen(QColor("#25607a"), 2))
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, not scene_ref.read_only)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, not scene_ref.read_only)
        self.setFlag(
            QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges,
            not scene_ref.read_only,
        )
        if scene_ref.read_only:
            self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)

        label = QGraphicsSimpleTextItem(router.id, self)
        label_rect = label.boundingRect()
        label.setPos(-label_rect.width() / 2, -label_rect.height() / 2)
        self.update_style(False)

    def update_style(self, link_start: bool = False) -> None:
        self.setBrush(QBrush(QColor("#d8f0ff") if self.scene_ref.is_route_reflector(self.router.id) else QColor("#f7f7f7")))
        color = QColor("#f2c94c") if link_start else QColor("#25607a")
        width = 4 if link_start else 2
        self.setPen(QPen(color, width))

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self.scene_ref.router_clicked(self):
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseDoubleClickEvent(self, event) -> None:
        self.scene_ref.router_double_clicked(self)
        event.accept()

    def itemChange(self, change: QGraphicsItem.GraphicsItemChange, value):
        if (
            change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged
            and not self.scene_ref.read_only
        ):
            point = self.pos()
            self.router.x = point.x()
            self.router.y = point.y()
            self.scene_ref.update_links()
            self.scene_ref.update_as_groups()
            self.scene_ref.mark_dirty()
        return super().itemChange(change, value)


class LinkItem(QGraphicsLineItem):
    ROUTE_CLIENT_COLOR = QColor("#1f9d55")
    DISABLED_ROUTE_CLIENT_COLOR = QColor("#78b98f")
    NORMAL_COLOR = QColor("#444444")
    DISABLED_COLOR = QColor("#b0b0b0")
    PATH_COLOR = QColor("#d93025")

    def __init__(self, link: LinkEdge, read_only: bool = False) -> None:
        super().__init__()
        self.link = link
        self.path_highlighted = False
        self.path_direction: Optional[tuple[str, str]] = None
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, not read_only)
        if read_only:
            self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        self.update_pen()

    def has_route_client(self) -> bool:
        return self.link.rr_client_from_a or self.link.rr_client_from_b

    def update_pen(self) -> None:
        if self.path_highlighted:
            self.setPen(QPen(self.PATH_COLOR, 3, Qt.PenStyle.SolidLine))
            self.setZValue(-0.5)
            return
        self.setZValue(-1)
        if self.has_route_client():
            color = self.ROUTE_CLIENT_COLOR if self.link.enabled else self.DISABLED_ROUTE_CLIENT_COLOR
        elif not self.link.enabled:
            color = self.DISABLED_COLOR
        else:
            color = self.NORMAL_COLOR
        pen = QPen(color, 2, Qt.PenStyle.SolidLine if self.link.enabled else Qt.PenStyle.DashLine)
        self.setPen(pen)

    def set_path_highlighted(self, direction: Optional[tuple[str, str]]) -> None:
        self.path_direction = direction
        self.path_highlighted = direction is not None
        self.update_pen()
        self.update()

    def boundingRect(self) -> QRectF:
        return super().boundingRect().adjusted(-8, -8, 8, 8)

    def paint(self, painter: QPainter, option, widget=None) -> None:
        super().paint(painter, option, widget)
        if self.path_direction is not None:
            start_id, end_id = self.path_direction
            if start_id == self.link.a and end_id == self.link.b:
                start = self.line().p1()
                end = self.line().p2()
            elif start_id == self.link.b and end_id == self.link.a:
                start = self.line().p2()
                end = self.line().p1()
            else:
                return

            painter.save()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QBrush(self.PATH_COLOR))
            self._paint_arrow(painter, start, end, 0.5)
            painter.restore()
            return

        if not self.has_route_client():
            return

        painter.save()
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QBrush(self.pen().color()))
        if self.link.rr_client_from_a:
            self._paint_arrow(painter, self.line().p1(), self.line().p2(), 0.62)
        if self.link.rr_client_from_b:
            self._paint_arrow(painter, self.line().p2(), self.line().p1(), 0.38)
        painter.restore()

    def _paint_arrow(
        self,
        painter: QPainter,
        start: QPointF,
        end: QPointF,
        fraction: float,
    ) -> None:
        dx = end.x() - start.x()
        dy = end.y() - start.y()
        length = math.hypot(dx, dy)
        if length < 1.0:
            return

        ux = dx / length
        uy = dy / length
        tip = QPointF(start.x() + dx * fraction, start.y() + dy * fraction)
        arrow_len = 11.0 if self.path_highlighted else 14.0
        arrow_half_width = 4.5 if self.path_highlighted else 6.0
        base = QPointF(tip.x() - ux * arrow_len, tip.y() - uy * arrow_len)
        normal_x = -uy
        normal_y = ux
        left = QPointF(
            base.x() + normal_x * arrow_half_width,
            base.y() + normal_y * arrow_half_width,
        )
        right = QPointF(
            base.x() - normal_x * arrow_half_width,
            base.y() - normal_y * arrow_half_width,
        )
        painter.drawPolygon(QPolygonF([tip, left, right]))


class LineLegendSample(QWidget):
    def __init__(
        self,
        color: QColor,
        dashed: bool = False,
        arrow: bool = False,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self.color = QColor(color)
        self.dashed = dashed
        self.arrow = arrow
        self.setFixedSize(58, 18)

    def paintEvent(self, event) -> None:
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen_style = Qt.PenStyle.DashLine if self.dashed else Qt.PenStyle.SolidLine
        painter.setPen(QPen(self.color, 2, pen_style))
        y = self.height() / 2
        start = QPointF(5, y)
        end = QPointF(self.width() - 6, y)
        painter.drawLine(start, end)

        if self.arrow:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QBrush(self.color))
            tip = QPointF(self.width() - 12, y)
            base_x = tip.x() - 12
            painter.drawPolygon(
                QPolygonF(
                    [
                        tip,
                        QPointF(base_x, y - 5),
                        QPointF(base_x, y + 5),
                    ]
                )
            )


class LinkLegendWidget(QFrame):
    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setObjectName("linkLegend")
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setStyleSheet(
            """
            QFrame#linkLegend {
                background: rgba(255, 255, 255, 225);
                border: 1px solid #c8d1dc;
                border-radius: 6px;
            }
            QLabel {
                color: #1f2933;
                font-size: 12px;
            }
            QLabel#legendTitle {
                font-weight: 600;
            }
            """
        )

        layout = QVBoxLayout()
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setSpacing(5)

        title = QLabel("Legend")
        title.setObjectName("legendTitle")
        layout.addWidget(title)

        self._add_row(layout, LinkItem.NORMAL_COLOR, False, False, "Normal link")
        self._add_row(layout, LinkItem.DISABLED_COLOR, True, False, "Disabled link")
        self._add_row(layout, LinkItem.PATH_COLOR, False, True, "Selected best path")
        self._add_row(
            layout,
            LinkItem.ROUTE_CLIENT_COLOR,
            False,
            True,
            "RR -> Client",
        )
        self._add_row(
            layout,
            LinkItem.DISABLED_ROUTE_CLIENT_COLOR,
            True,
            True,
            "Disabled RR -> Client",
        )
        self.setLayout(layout)

    def _add_row(
        self,
        layout: QVBoxLayout,
        color: QColor,
        dashed: bool,
        arrow: bool,
        label: str,
    ) -> None:
        row = QHBoxLayout()
        row.setSpacing(7)
        row.addWidget(LineLegendSample(color, dashed, arrow, self))
        row.addWidget(QLabel(label))
        layout.addLayout(row)


class DirtyIndicatorWidget(QFrame):
    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setObjectName("dirtyIndicator")
        self.setStyleSheet(
            """
            QFrame#dirtyIndicator {
                background: rgba(255, 248, 220, 235);
                border: 1px solid #d9a441;
                border-radius: 6px;
            }
            QLabel {
                color: #6f4f00;
                font-size: 12px;
                font-weight: 600;
            }
            """
        )

        layout = QHBoxLayout()
        layout.setContentsMargins(10, 6, 10, 6)
        layout.addWidget(QLabel("Unsaved changes"))
        self.setLayout(layout)
        self.hide()


class AsGroupItem(QGraphicsRectItem):
    def __init__(self, asn: int) -> None:
        super().__init__()
        self.asn = asn
        color = color_for_asn(asn)
        fill = QColor(color)
        fill.setAlpha(18)
        self.setBrush(QBrush(fill))
        self.setPen(QPen(color, 2, Qt.PenStyle.DashLine))
        self.setZValue(-3)

        self.label = QGraphicsSimpleTextItem(f"AS {asn}", self)
        self.label.setBrush(QBrush(color))

    def update_rect(self, rect: QRectF) -> None:
        self.setRect(rect)
        self.label.setPos(rect.left() + 10, rect.top() + 6)


class TopologyScene(QGraphicsScene):
    def __init__(self, model: TopologyModel, read_only: bool = False) -> None:
        super().__init__()
        self.model = model
        self.read_only = read_only
        self.router_items: dict[str, RouterItem] = {}
        self.link_items: list[LinkItem] = []
        self.as_group_items: dict[int, AsGroupItem] = {}
        self.highlighted_path: list[str] = []
        self.main_window: Optional["MainWindow"] = None
        self.setSceneRect(-2000, -2000, 4000, 4000)
        self.rebuild()

    def rebuild(self) -> None:
        self.clear()
        self.router_items.clear()
        self.link_items.clear()
        self.as_group_items.clear()
        for link in self.model.links:
            item = LinkItem(link, read_only=self.read_only)
            item.setZValue(-1)
            self.link_items.append(item)
            self.addItem(item)
        for router in self.model.routers.values():
            item = RouterItem(router, self)
            self.router_items[router.id] = item
            self.addItem(item)
        self.update_links()
        self.set_highlighted_path(self.highlighted_path)
        self.update_as_groups()

    def update_links(self) -> None:
        for item in self.link_items:
            a_item = self.router_items.get(item.link.a)
            b_item = self.router_items.get(item.link.b)
            if not a_item or not b_item:
                continue
            item.setLine(QLineF(a_item.pos(), b_item.pos()))
            item.update_pen()

    def is_route_reflector(self, router_id: str) -> bool:
        for link in self.model.links:
            if link.a == router_id and link.rr_client_from_a:
                return True
            if link.b == router_id and link.rr_client_from_b:
                return True
        return False

    def update_as_groups(self) -> None:
        grouped: dict[int, list[RouterItem]] = {}
        for item in self.router_items.values():
            grouped.setdefault(item.router.asn, []).append(item)

        for asn in list(self.as_group_items):
            if asn not in grouped:
                self.removeItem(self.as_group_items[asn])
                del self.as_group_items[asn]

        for asn, router_items in grouped.items():
            group_item = self.as_group_items.get(asn)
            if group_item is None:
                group_item = AsGroupItem(asn)
                self.as_group_items[asn] = group_item
                self.addItem(group_item)

            rect: Optional[QRectF] = None
            for router_item in router_items:
                item_rect = router_item.mapRectToScene(router_item.boundingRect())
                rect = item_rect if rect is None else rect.united(item_rect)
            if rect is not None:
                group_item.update_rect(rect.adjusted(-34, -30, 34, 30))

    def router_double_clicked(self, item: RouterItem) -> None:
        if (
            self.main_window is not None
            and not self.read_only
            and not self.main_window.link_mode_enabled
        ):
            self.main_window.edit_router_item(item)

    def router_clicked(self, item: RouterItem) -> bool:
        if self.main_window is None:
            return False
        if self.read_only:
            return False
        return self.main_window.handle_router_click_for_link_mode(item)

    def mark_dirty(self) -> None:
        if not self.read_only and self.main_window is not None:
            self.main_window.mark_dirty()

    def set_link_start_router(self, router_id: Optional[str]) -> None:
        for item in self.router_items.values():
            item.update_style(item.router.id == router_id)

    def set_highlighted_path(self, hops: list[str]) -> None:
        self.highlighted_path = list(hops)
        highlighted_edges = {
            tuple(sorted((hops[index], hops[index + 1]))): (
                hops[index],
                hops[index + 1],
            )
            for index in range(len(hops) - 1)
        }
        for item in self.link_items:
            edge = tuple(sorted((item.link.a, item.link.b)))
            item.set_path_highlighted(highlighted_edges.get(edge))


class TopologyView(QGraphicsView):
    def __init__(self, scene: TopologyScene) -> None:
        super().__init__(scene)
        self._panning = False
        self._last_pan_pos = QPoint()
        self._zoom_factor = 1.15
        self._min_zoom = 0.2
        self._max_zoom = 4.0
        self._current_zoom = 1.0
        self._drag_mode_before_pan = QGraphicsView.DragMode.RubberBandDrag
        self.legend_widget: Optional[QWidget] = None
        self.dirty_indicator: Optional[QWidget] = None
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self.setResizeAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self.setDragMode(QGraphicsView.DragMode.RubberBandDrag)

    def set_legend_widget(self, widget: QWidget) -> None:
        self.legend_widget = widget
        self._position_overlays()

    def set_dirty_indicator(self, widget: QWidget) -> None:
        self.dirty_indicator = widget
        self._position_overlays()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._position_overlays()

    def scrollContentsBy(self, dx: int, dy: int) -> None:
        super().scrollContentsBy(dx, dy)
        self._position_overlays()

    def _position_overlays(self) -> None:
        margin = 12
        if self.dirty_indicator is not None:
            self.dirty_indicator.adjustSize()
            self.dirty_indicator.move(margin, margin)
            self.dirty_indicator.raise_()
        if self.legend_widget is not None:
            self.legend_widget.adjustSize()
            x = max(margin, self.width() - self.legend_widget.width() - margin)
            self.legend_widget.move(x, margin)
            self.legend_widget.raise_()

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.RightButton:
            self._panning = True
            self._last_pan_pos = event.pos()
            self._drag_mode_before_pan = self.dragMode()
            self.setDragMode(QGraphicsView.DragMode.NoDrag)
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._panning:
            delta = event.pos() - self._last_pan_pos
            self._last_pan_pos = event.pos()
            self.horizontalScrollBar().setValue(self.horizontalScrollBar().value() - delta.x())
            self.verticalScrollBar().setValue(self.verticalScrollBar().value() - delta.y())
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.RightButton and self._panning:
            self._panning = False
            self.setDragMode(self._drag_mode_before_pan)
            self.unsetCursor()
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def wheelEvent(self, event) -> None:
        if event.angleDelta().y() == 0:
            event.ignore()
            return

        factor = self._zoom_factor if event.angleDelta().y() > 0 else 1 / self._zoom_factor
        next_zoom = self._current_zoom * factor
        if next_zoom < self._min_zoom or next_zoom > self._max_zoom:
            event.accept()
            return

        self._current_zoom = next_zoom
        self.scale(factor, factor)
        event.accept()


class ObserverClient(QObject):
    def __init__(self, window: "MainWindow", pipe_name: str) -> None:
        super().__init__(window)
        self.window = window
        self.pipe_name = pipe_name
        self.socket = QLocalSocket(self)
        self.buffer = bytearray()
        self.reconnect_timer = QTimer(self)
        self.stopped = False
        self.reconnect_timer.setInterval(1000)
        self.reconnect_timer.timeout.connect(self.connect)
        self.socket.connected.connect(self._connected)
        self.socket.disconnected.connect(self._disconnected)
        self.socket.readyRead.connect(self._ready_read)
        self.socket.errorOccurred.connect(self._error)

    def start(self) -> None:
        self.connect()

    def stop(self) -> None:
        if self.stopped:
            return
        self.stopped = True
        try:
            self.reconnect_timer.stop()
        except RuntimeError:
            pass
        self._disconnect_signals()
        try:
            if self.socket.state() != QLocalSocket.LocalSocketState.UnconnectedState:
                self.socket.abort()
            self.socket.deleteLater()
        except RuntimeError:
            pass

    def connect(self) -> None:
        if self.stopped:
            return
        if self.socket.state() != QLocalSocket.LocalSocketState.UnconnectedState:
            return
        self._show_status(f"Waiting for Simulator pipe: {self.pipe_name}")
        self.socket.connectToServer(self.pipe_name)

    def _connected(self) -> None:
        if self.stopped:
            return
        self.reconnect_timer.stop()
        self._show_status(f"Connected to Simulator pipe: {self.pipe_name}", 3000)

    def _disconnected(self) -> None:
        if self.stopped:
            return
        self._show_status("Simulator disconnected; reconnecting...")
        self.reconnect_timer.start()

    def _error(self, *_) -> None:
        if self.stopped:
            return
        if self.socket.state() != QLocalSocket.LocalSocketState.UnconnectedState:
            self.socket.abort()
        self.reconnect_timer.start()

    def _ready_read(self) -> None:
        if self.stopped:
            return
        self.buffer.extend(bytes(self.socket.readAll()))
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            if not line.strip():
                continue
            try:
                message = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                self._show_status(f"Invalid observer message: {exc}", 5000)
                continue
            try:
                self.window.handle_observer_message(message)
            except RuntimeError:
                self.stop()
                return

    def _show_status(self, message: str, timeout_ms: int = 0) -> None:
        if self.stopped:
            return
        try:
            self.window.statusBar().showMessage(message, timeout_ms)
        except RuntimeError:
            self.stop()

    def _disconnect_signals(self) -> None:
        for signal, slot in (
            (self.reconnect_timer.timeout, self.connect),
            (self.socket.connected, self._connected),
            (self.socket.disconnected, self._disconnected),
            (self.socket.readyRead, self._ready_read),
            (self.socket.errorOccurred, self._error),
        ):
            try:
                signal.disconnect(slot)
            except (TypeError, RuntimeError):
                pass


class MainWindow(QMainWindow):
    def __init__(self, observe_mode: bool = False, pipe_name: str = DEFAULT_OBSERVER_PIPE) -> None:
        super().__init__()
        self.observe_mode = observe_mode
        self.pipe_name = pipe_name
        self.setWindowTitle("TopoObserver" if observe_mode else "TopoGenerator")
        self.resize(1100, 760)
        self.settings = QSettings("BgpTest", "TopoGenerator")
        self.current_topology_path: Optional[Path] = None
        self.dirty = False
        self.model = TopologyModel()
        self.edit_model_snapshot: Optional[TopologyModel] = None
        self.edit_topology_path: Optional[Path] = None
        self.edit_dirty = False
        self.best_paths: dict[tuple[str, str], dict] = {}
        self.link_mode_enabled = False
        self.pending_link_router_id: Optional[str] = None
        self.toolbar: Optional[QToolBar] = None
        self.link_mode_action: Optional[QAction] = None
        self.router_combo: Optional[QComboBox] = None
        self.prefix_combo: Optional[QComboBox] = None
        self.observer_client: Optional[ObserverClient] = None
        self.scene = TopologyScene(self.model, read_only=observe_mode)
        self.scene.main_window = self
        self.view = TopologyView(self.scene)
        self.view.setRenderHints(self.view.renderHints())
        self.setCentralWidget(self.view)
        self.link_legend = LinkLegendWidget(self.view)
        self.link_legend.show()
        self.view.set_legend_widget(self.link_legend)
        self.dirty_indicator = DirtyIndicatorWidget(self.view)
        self.view.set_dirty_indicator(self.dirty_indicator)
        self._build_toolbar()
        if observe_mode:
            self.dirty_indicator.hide()
            self.observer_client = ObserverClient(self, self.pipe_name)
            self.observer_client.start()
        else:
            self._restore_last_topology()

    def closeEvent(self, event) -> None:
        if self.observer_client is not None:
            self.observer_client.stop()
            self.observer_client = None
        super().closeEvent(event)

    def _build_toolbar(self) -> None:
        if self.toolbar is not None:
            self.removeToolBar(self.toolbar)
            self.toolbar.deleteLater()

        self.link_mode_action = None
        self.router_combo = None
        self.prefix_combo = None

        toolbar = QToolBar("Topology", self)
        self.toolbar = toolbar
        self.addToolBar(toolbar)

        if self.observe_mode:
            toolbar.addWidget(QLabel("Router"))
            self.router_combo = self._observer_combo("Search router")
            self.router_combo.currentTextChanged.connect(self.update_observed_path)
            toolbar.addWidget(self.router_combo)

            toolbar.addWidget(QLabel("Prefix"))
            self.prefix_combo = self._observer_combo("Search prefix")
            self.prefix_combo.currentTextChanged.connect(self.update_observed_path)
            toolbar.addWidget(self.prefix_combo)

            stop_action = QAction("Stop Observe", toolbar)
            stop_action.triggered.connect(self.stop_observe_mode)
            toolbar.addAction(stop_action)
            return

        add_router_action = QAction("Add Router", toolbar)
        add_router_action.triggered.connect(self.add_router)
        toolbar.addAction(add_router_action)

        self.link_mode_action = QAction("Link Mode (Q)", toolbar)
        self.link_mode_action.setCheckable(True)
        self.link_mode_action.setShortcut("Q")
        self.link_mode_action.toggled.connect(self.set_link_mode)
        toolbar.addAction(self.link_mode_action)

        for text, shortcut, callback in [
            ("New Topo", "", self.new_topo),
            ("Edit (E)", "E", self.edit_selected),
            ("Delete (Del)", "Del", self.delete_selected),
            ("Load", "", self.load_json),
            ("Save (Ctrl+S)", "Ctrl+S", self.save_json),
            ("Export", "", self.export_json),
            ("Observe", "", self.start_observe_mode),
            ("BatchUpdateAll", "", self.batch_update_all),
        ]:
            action = QAction(text, toolbar)
            if shortcut:
                action.setShortcut(shortcut)
            action.triggered.connect(callback)
            toolbar.addAction(action)

    def _observer_combo(self, placeholder: str) -> QComboBox:
        combo = QComboBox()
        combo.setEditable(True)
        combo.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        combo.setMaxVisibleItems(20)
        combo.lineEdit().setPlaceholderText(placeholder)
        completer = QCompleter(combo.model(), combo)
        completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        completer.setFilterMode(Qt.MatchFlag.MatchContains)
        combo.setCompleter(completer)
        combo.observer_completer = completer
        return combo

    def add_router(self) -> None:
        if self.observe_mode:
            return
        index = self._next_router_index()
        center = self.view.mapToScene(self.view.viewport().rect().center())
        offset = 24 * (len(self.model.routers) % 6)
        try:
            router_id = router_id_from_index(index)
        except ValueError as exc:
            self._error(str(exc))
            return
        router = RouterNode(
            id=f"R{index}",
            router_id=router_id,
            asn=65000,
            cluster_id=router_id,
            x=center.x() + offset,
            y=center.y() + offset,
        )
        self.model.add_router(router)
        self.scene.rebuild()
        self.clear_pending_link()
        if router.id in self.scene.router_items:
            self.scene.clearSelection()
            self.scene.router_items[router.id].setSelected(True)
        self.mark_dirty()

    def edit_selected(self) -> None:
        if self.observe_mode:
            return
        selected = self.scene.selectedItems()
        if not selected:
            return
        router_items = [item for item in selected if isinstance(item, RouterItem)]
        if router_items:
            if len(router_items) == 1:
                self.edit_router_item(router_items[0])
            else:
                self.edit_router_items_asn(router_items)
            return

        link_items = [item for item in selected if isinstance(item, LinkItem)]
        if len(link_items) != 1:
            self.scene.clearSelection()
            self.clear_pending_link()
            return

        item = link_items[0]
        if isinstance(item, LinkItem):
            dialog = LinkDialog(sorted(self.model.routers.keys()), item.link, self)
            if dialog.exec() == QDialog.DialogCode.Accepted:
                updated = dialog.link()
                item.link.a = updated.a
                item.link.b = updated.b
                item.link.enabled = updated.enabled
                item.link.delay_ms = updated.delay_ms
                item.link.rr_client_from_a = updated.rr_client_from_a
                item.link.rr_client_from_b = updated.rr_client_from_b
                item.link.mrai_ms_from_a = updated.mrai_ms_from_a
                item.link.mrai_ms_from_b = updated.mrai_ms_from_b
                self.scene.rebuild()
                self.clear_pending_link()
                self.mark_dirty()

    def edit_router_items_asn(self, items: list[RouterItem]) -> None:
        routers = [item.router for item in items]
        dialog = BatchRouterAsnDialog(routers, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return

        asn = dialog.asn()
        selected_ids = [router.id for router in routers]
        for router in routers:
            router.asn = asn
        self.scene.rebuild()
        self.clear_pending_link()
        for router_id in selected_ids:
            if router_id in self.scene.router_items:
                self.scene.router_items[router_id].setSelected(True)
        self.mark_dirty()

    def edit_router_item(self, item: RouterItem) -> None:
        dialog = RouterDialog(item.router, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return

        old_id = item.router.id
        updated = dialog.router(item.router.x, item.router.y)
        if not updated.id:
            self._error("Router id is required")
            return
        if updated.id != old_id and updated.id in self.model.routers:
            self._error(f"Router id already exists: {updated.id}")
            return

        for link in self.model.links:
            if link.a == old_id:
                link.a = updated.id
            if link.b == old_id:
                link.b = updated.id
        self.model.routers.pop(old_id, None)
        self.model.add_router(updated)
        self.scene.rebuild()
        self.clear_pending_link()
        if updated.id in self.scene.router_items:
            self.scene.router_items[updated.id].setSelected(True)
        self.mark_dirty()

    def delete_selected(self) -> None:
        if self.observe_mode:
            return
        selected = self.scene.selectedItems()
        if not selected:
            return
        item = selected[0]
        if isinstance(item, RouterItem):
            self.model.remove_router(item.router.id)
        elif isinstance(item, LinkItem):
            self.model.remove_link(item.link.a, item.link.b)
        self.scene.rebuild()
        self.clear_pending_link()
        self.mark_dirty()

    def batch_update_all(self) -> None:
        if self.observe_mode:
            return
        if not self.model.links:
            self.statusBar().showMessage("No links to update.", 3000)
            return

        dialog = BatchUpdateAllDialog(self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return

        for link in self.model.links:
            link.delay_ms = dialog.delay_ms()
            link.mrai_ms_from_a = dialog.mrai_ms()
            link.mrai_ms_from_b = dialog.mrai_ms()

        self.scene.update_links()
        self.mark_dirty()
        self.statusBar().showMessage(
            f"Updated delay and MRAI for {len(self.model.links)} links.",
            3000,
        )

    def start_observe_mode(self) -> None:
        if self.observe_mode:
            return

        self.edit_model_snapshot = TopologyModel.from_json(self.model.to_json())
        self.edit_topology_path = self.current_topology_path
        self.edit_dirty = self.dirty
        self.set_link_mode(False)
        self.best_paths.clear()
        self.observe_mode = True
        self.setWindowTitle("TopoObserver")
        self.set_dirty(False)
        self.dirty_indicator.hide()
        self._build_toolbar()
        self._replace_model(TopologyModel())
        self.observer_client = ObserverClient(self, self.pipe_name)
        self.observer_client.start()

    def stop_observe_mode(self) -> None:
        if not self.observe_mode:
            return

        if self.observer_client is not None:
            self.observer_client.stop()
            self.observer_client = None

        observed_model = self.model
        observed_path = self.current_topology_path
        restored_model = self.edit_model_snapshot or observed_model
        restored_path = self.edit_topology_path if self.edit_model_snapshot is not None else observed_path
        restored_dirty = self.edit_dirty if self.edit_model_snapshot is not None else False

        self.edit_model_snapshot = None
        self.edit_topology_path = None
        self.edit_dirty = False
        self.best_paths.clear()
        self.observe_mode = False
        self.setWindowTitle("TopoGenerator")
        self._build_toolbar()
        self._replace_model(restored_model)
        self.current_topology_path = restored_path
        self.set_dirty(restored_dirty)
        self.statusBar().showMessage("Observation stopped.", 3000)

    def new_topo(self) -> None:
        if self.observe_mode:
            return
        start_dir = self.current_topology_path.parent if self.current_topology_path else Path.cwd()
        default_path = start_dir / "new_topology.json"
        path, _ = QFileDialog.getSaveFileName(self, "New topology", str(default_path), "JSON (*.json)")
        if not path:
            return

        topology_path = Path(path)
        if topology_path.suffix.lower() != ".json":
            topology_path = topology_path.with_suffix(".json")

        model = TopologyModel()
        try:
            self._write_topology_model(topology_path, model)
        except OSError as exc:
            self._error(f"Failed to create topology: {exc}")
            return

        self._replace_model(model)
        self._remember_topology(topology_path)
        self.set_dirty(False)
        self.statusBar().showMessage(f"New topology: {topology_path}", 3000)

    def load_json(self) -> None:
        if self.observe_mode:
            return
        start_dir = str(self.current_topology_path.parent) if self.current_topology_path else ""
        path, _ = QFileDialog.getOpenFileName(self, "Load topology", start_dir, "JSON (*.json)")
        if not path:
            return
        try:
            self._load_topology(Path(path))
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
            self._error(f"Failed to load topology: {exc}")

    def export_json(self) -> None:
        if self.observe_mode:
            return
        default_path = str(self.current_topology_path) if self.current_topology_path else "topology.json"
        path, _ = QFileDialog.getSaveFileName(self, "Export topology", default_path, "JSON (*.json)")
        if not path:
            return
        topology_path = Path(path)
        try:
            self._write_topology(topology_path)
            self._remember_topology(topology_path)
            self.set_dirty(False)
        except OSError as exc:
            self._error(f"Failed to export topology: {exc}")

    def save_json(self) -> None:
        if self.observe_mode:
            return
        if self.current_topology_path is None:
            self.export_json()
            return
        try:
            self._write_topology(self.current_topology_path)
            self.set_dirty(False)
            self.statusBar().showMessage(f"Saved topology: {self.current_topology_path}", 3000)
        except OSError as exc:
            self._error(f"Failed to save topology: {exc}")

    def _write_topology(self, path: Path) -> None:
        self._write_topology_model(path, self.model)

    def _write_topology_model(self, path: Path, model: TopologyModel) -> None:
        with path.open("w", encoding="utf-8") as handle:
            json.dump(model.to_json(), handle, indent=2)
            handle.write("\n")

    def _replace_model(self, model: TopologyModel) -> None:
        self.model = model
        self.scene = TopologyScene(self.model, read_only=self.observe_mode)
        self.scene.main_window = self
        self.view.setScene(self.scene)
        self.clear_pending_link()
        if self.observe_mode:
            self.refresh_observer_choices()

    def _load_topology(self, path: Path) -> None:
        with path.open("r", encoding="utf-8") as handle:
            self._replace_model(TopologyModel.from_json(json.load(handle)))
        self._remember_topology(path)
        self.set_dirty(False)

    def mark_dirty(self) -> None:
        if self.observe_mode:
            return
        self.set_dirty(True)

    def set_dirty(self, dirty: bool) -> None:
        self.dirty = dirty
        self.dirty_indicator.setVisible(dirty)
        self.view._position_overlays()

    def _remember_topology(self, path: Path) -> None:
        self.current_topology_path = path
        self.settings.setValue("last_topology", str(path))
        self.statusBar().showMessage(f"Topology: {path}", 3000)

    def _restore_last_topology(self) -> None:
        if self.observe_mode:
            return
        path_text = self.settings.value("last_topology", "", str)
        if not path_text:
            return
        path = Path(path_text)
        if not path.is_file():
            self.settings.remove("last_topology")
            return
        try:
            self._load_topology(path)
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
            self.settings.remove("last_topology")
            self.statusBar().showMessage(f"Failed to restore topology: {exc}", 5000)

    def _error(self, message: str) -> None:
        QMessageBox.critical(self, "TopoGenerator", message)

    def set_link_mode(self, enabled: bool) -> None:
        if self.observe_mode and enabled:
            return
        self.link_mode_enabled = enabled
        self.clear_pending_link()
        if self.link_mode_action and self.link_mode_action.isChecked() != enabled:
            self.link_mode_action.setChecked(enabled)
        if enabled:
            self.statusBar().showMessage("Link mode: click two routers to create a link.")
        else:
            self.statusBar().clearMessage()

    def clear_pending_link(self) -> None:
        self.pending_link_router_id = None
        self.scene.set_link_start_router(None)

    def handle_router_click_for_link_mode(self, item: RouterItem) -> bool:
        if not self.link_mode_enabled:
            return False

        router_id = item.router.id
        if self.pending_link_router_id is None or self.pending_link_router_id == router_id:
            self.pending_link_router_id = router_id
            self.scene.set_link_start_router(router_id)
            self.statusBar().showMessage(f"Link mode: selected {router_id}; click another router.")
            return True

        link = LinkEdge(
            a=self.pending_link_router_id,
            b=router_id,
            enabled=True,
            delay_ms=0,
        )
        try:
            self.model.add_link(link)
        except ValueError as exc:
            self.statusBar().showMessage(str(exc), 4000)
            self.clear_pending_link()
            return True

        self.pending_link_router_id = None
        self.scene.rebuild()
        self.statusBar().showMessage(f"Created link {link.a} - {link.b}.", 3000)
        self.mark_dirty()
        return True

    def _next_router_index(self) -> int:
        index = len(self.model.routers) + 1
        while f"R{index}" in self.model.routers:
            index += 1
        return index

    def handle_observer_message(self, message: dict) -> None:
        message_type = message.get("type")
        if message_type == "topology":
            self.best_paths.clear()
            topology_path = str(message.get("topology_path", "")).strip()
            if topology_path:
                try:
                    self._load_observer_topology_from_path(Path(topology_path))
                    self.statusBar().showMessage(
                        f"Topology loaded from Simulator path: {topology_path}",
                        3000,
                    )
                    return
                except (OSError, json.JSONDecodeError, ValueError) as exc:
                    self.statusBar().showMessage(
                        f"Failed to load topology path; using Simulator snapshot: {exc}",
                        5000,
                    )
            topology = message.get("topology", {})
            self._replace_model(TopologyModel.from_json(topology))
            self.statusBar().showMessage("Topology loaded from Simulator.", 3000)
            return
        if message_type == "best_path":
            router = str(message.get("router", ""))
            prefix = str(message.get("prefix", ""))
            if not router or not prefix:
                return
            self.best_paths[(router, prefix)] = message
            self.refresh_observer_choices()
            self.update_observed_path()

    def _load_observer_topology_from_path(self, path: Path) -> None:
        with path.open("r", encoding="utf-8") as handle:
            self._replace_model(TopologyModel.from_json(json.load(handle)))
        self.current_topology_path = path

    def refresh_observer_choices(self) -> None:
        if self.router_combo is None or self.prefix_combo is None:
            return

        self._replace_combo_items_preserving_text(
            self.router_combo,
            sorted(self.model.routers, key=natural_sort_key),
        )

        prefixes = {
            prefix
            for router in self.model.routers.values()
            for prefix in router.originated_prefixes
        }
        prefixes.update(prefix for _, prefix in self.best_paths)
        self._replace_combo_items_preserving_text(
            self.prefix_combo,
            sorted(prefixes, key=natural_sort_key),
        )

    def _replace_combo_items_preserving_text(
        self,
        combo: QComboBox,
        items: list[str],
    ) -> None:
        line_edit = combo.lineEdit() if combo.isEditable() else None
        text = line_edit.text() if line_edit is not None else combo.currentText()
        cursor_position = line_edit.cursorPosition() if line_edit is not None else 0
        current_items = [combo.itemText(index) for index in range(combo.count())]

        signals_were_blocked = combo.blockSignals(True)
        if current_items != items:
            combo.clear()
            combo.addItems(items)

        index = combo.findText(text, Qt.MatchFlag.MatchExactly)
        if text and index >= 0:
            combo.setCurrentIndex(index)
        elif combo.isEditable():
            combo.setEditText(text)

        if line_edit is not None:
            line_edit.setCursorPosition(min(cursor_position, len(text)))
        combo.blockSignals(signals_were_blocked)

    def update_observed_path(self) -> None:
        if not self.observe_mode or self.router_combo is None or self.prefix_combo is None:
            return
        router = self.router_combo.currentText().strip()
        prefix = self.prefix_combo.currentText().strip()
        if not router or not prefix:
            self.scene.set_highlighted_path([])
            return

        hops = self.best_path_hops(router, prefix)
        if not hops:
            self.scene.set_highlighted_path([])
            self.statusBar().showMessage(f"No valid best path for {router} -> {prefix}")
            return

        self.scene.set_highlighted_path(hops)
        self.statusBar().showMessage(
            f"{router} -> {prefix}: {' -> '.join(hops)}"
        )

    def best_path_hops(self, router: str, prefix: str) -> list[str]:
        hops: list[str] = []
        visited: set[str] = set()
        current = router

        while current and current not in visited:
            visited.add(current)
            hops.append(current)
            best_path = self.best_paths.get((current, prefix))
            if not best_path or not best_path.get("valid", False):
                return []
            if best_path.get("local_origin", False):
                return hops

            learned_from = str(best_path.get("learned_from", "")).strip()
            if not learned_from or learned_from == current:
                return []
            current = learned_from

        return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--observe", action="store_true")
    parser.add_argument("--pipe", default=DEFAULT_OBSERVER_PIPE)
    args, qt_args = parser.parse_known_args()

    app = QApplication([sys.argv[0], *qt_args])
    window = MainWindow(observe_mode=args.observe, pipe_name=args.pipe)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
