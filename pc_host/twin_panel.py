from __future__ import annotations

from PyQt5 import QtCore, QtGui, QtWidgets

from protocol import led_bits


SEGMENTS = {
    "0": "abcedf".replace("e", "e"),
    "1": "bc",
    "2": "abged",
    "3": "abgcd",
    "4": "fgbc",
    "5": "afgcd",
    "6": "afgecd",
    "7": "abc",
    "8": "abcdefg",
    "9": "abfgcd",
    "A": "abcefg",
    "B": "fgecd",
    "C": "afed",
    "D": "bgecd",
    "E": "afged",
    "F": "afge",
    "G": "afecd",
    "H": "fbceg",
    "I": "bc",
    "J": "bcde",
    "L": "fed",
    "N": "abcef",
    "O": "abcdef",
    "P": "abfeg",
    "R": "eg",
    "S": "afgcd",
    "T": "fedg",
    "U": "fbcde",
    "Y": "fbgcd",
    "-": "g",
    "_": "d",
    " ": "",
}


class SevenSegmentDigit(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.char = " "
        self.dp = False
        self.setMinimumSize(42, 72)

    def set_value(self, char: str, dp: bool) -> None:
        self.char = char[:1] if char else " "
        self.dp = dp
        self.update()

    def paintEvent(self, event):  # noqa: N802
        del event
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        rect = self.rect().adjusted(4, 4, -4, -4)
        active = QtGui.QColor("#ff3b30")
        inactive = QtGui.QColor("#3a1f1f")
        segments = SEGMENTS.get(self.char.upper(), SEGMENTS.get(self.char, ""))

        w = rect.width()
        h = rect.height()
        t = max(4, min(w, h) // 9)
        x = rect.x()
        y = rect.y()
        coordinates = {
            "a": QtCore.QRectF(x + t, y, w - 2 * t, t),
            "b": QtCore.QRectF(x + w - t, y + t, t, h // 2 - t),
            "c": QtCore.QRectF(x + w - t, y + h // 2, t, h // 2 - t),
            "d": QtCore.QRectF(x + t, y + h - t, w - 2 * t, t),
            "e": QtCore.QRectF(x, y + h // 2, t, h // 2 - t),
            "f": QtCore.QRectF(x, y + t, t, h // 2 - t),
            "g": QtCore.QRectF(x + t, y + h // 2 - t // 2, w - 2 * t, t),
        }
        for name, seg_rect in coordinates.items():
            painter.setBrush(active if name in segments else inactive)
            painter.setPen(QtCore.Qt.NoPen)
            painter.drawRoundedRect(seg_rect, 2, 2)

        painter.setBrush(active if self.dp else inactive)
        painter.drawEllipse(QtCore.QPointF(x + w + 2, y + h - t), t / 1.8, t / 1.8)


class LedIndicator(QtWidgets.QWidget):
    def __init__(self, label: str, parent=None):
        super().__init__(parent)
        self.label = label
        self.on = False
        self.setFixedSize(46, 54)

    def set_on(self, on: bool) -> None:
        self.on = on
        self.update()

    def paintEvent(self, event):  # noqa: N802
        del event
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        color = QtGui.QColor("#24c45a" if self.on else "#263128")
        painter.setBrush(color)
        painter.setPen(QtGui.QPen(QtGui.QColor("#5b6b60")))
        painter.drawEllipse(13, 4, 20, 20)
        painter.setPen(QtGui.QColor("#d7ded9"))
        painter.drawText(QtCore.QRect(0, 30, self.width(), 18), QtCore.Qt.AlignCenter, self.label)


class TwinPanel(QtWidgets.QWidget):
    key_clicked = QtCore.pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.digits = [SevenSegmentDigit() for _ in range(8)]
        self.leds = [LedIndicator(f"L{i}") for i in range(8)]
        self.buttons: dict[str, QtWidgets.QPushButton] = {}
        self._highlight_timers: dict[str, QtCore.QTimer] = {}
        self._build()

    def _build(self) -> None:
        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(12)

        digit_row = QtWidgets.QHBoxLayout()
        digit_row.setSpacing(8)
        for digit in self.digits:
            digit_row.addWidget(digit)
        root.addLayout(digit_row)

        led_row = QtWidgets.QHBoxLayout()
        led_row.setSpacing(8)
        for led in self.leds:
            led_row.addWidget(led)
        root.addLayout(led_row)

        key_grid = QtWidgets.QGridLayout()
        key_grid.setHorizontalSpacing(8)
        key_grid.setVerticalSpacing(8)
        names = ["FUNC", "SHIFT", "ADD", "SAVE", "DISP", "SPEED", "FORMAT", "EXT", "USER1", "USER2"]
        for i, name in enumerate(names):
            button = QtWidgets.QPushButton(name)
            button.setMinimumHeight(34)
            button.clicked.connect(lambda checked=False, n=name: self.key_clicked.emit(n))
            self.buttons[name] = button
            key_grid.addWidget(button, i // 5, i % 5)
        root.addLayout(key_grid)

    def update_display(self, chars: str, dp_mask: int) -> None:
        chars = chars.ljust(8)[:8]
        for i, digit in enumerate(self.digits):
            digit.set_value(chars[i], bool(dp_mask & (1 << i)))

    def update_leds(self, value: int) -> None:
        for led, on in zip(self.leds, led_bits(value)):
            led.set_on(on)

    def highlight_key(self, name: str) -> None:
        button = self.buttons.get(name.upper())
        if button is None:
            return
        button.setProperty("activeKey", True)
        button.setStyleSheet("QPushButton[activeKey='true'] { background: #2d7dd2; color: white; }")
        timer = self._highlight_timers.get(name)
        if timer is None:
            timer = QtCore.QTimer(self)
            timer.setSingleShot(True)
            self._highlight_timers[name] = timer
        try:
            timer.timeout.disconnect()
        except TypeError:
            pass
        timer.timeout.connect(lambda b=button: self._clear_highlight(b))
        timer.start(200)

    @staticmethod
    def _clear_highlight(button: QtWidgets.QPushButton) -> None:
        button.setProperty("activeKey", False)
        button.setStyleSheet("")
