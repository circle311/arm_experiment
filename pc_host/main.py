from __future__ import annotations

import csv
import sys
from datetime import datetime
from pathlib import Path

from astral import LocationInfo
from astral.sun import sun
import matplotlib.pyplot as plt
from PyQt5 import QtCore, QtGui, QtWidgets

from ntp_helper import commands_for_datetime, get_ntp_time
from protocol import (
    AlarmEvent,
    DispEvent,
    EditEvent,
    ErrorResponse,
    KeyEvent,
    LedEvent,
    ModeEvent,
    OkResponse,
    PongEvent,
    RxEcho,
    UnknownLine,
    parse_line,
)
from serial_worker import SerialWorker
from twin_panel import TwinPanel
from weather_helper import WeatherData, commands_for_weather, get_weather


APP_DIR = Path(__file__).resolve().parent
EVENT_CSV = APP_DIR / "events.csv"
LOG_TX = "#1f6feb"
LOG_RX = "#2da44e"
LOG_EVT = "#8250df"
LOG_ERR = "#cf222e"


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("S800 Smart Clock PC Host")
        self.resize(1180, 760)
        self.worker = SerialWorker(self)
        self.pending_tests: list[tuple[str, str, bool]] = []
        self.current_test: tuple[str, str, bool] | None = None
        self.test_results: list[tuple[str, bool, str]] = []
        self.test_serial = 0
        self.command_queue: list[str] = []
        self.queue_active = False
        self.last_weather: WeatherData | None = None
        self._build_ui()
        self._connect_signals()
        self.refresh_ports()
        self.statusBar().showMessage("Disconnected | FORMAT LEFT | MODE DAY | ALARM OFF")
        self.weather_timer = QtCore.QTimer(self)
        self.weather_timer.timeout.connect(self.fetch_weather)
        self.weather_timer.start(30 * 60 * 1000)

    def _build_ui(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QGridLayout(central)
        root.setColumnStretch(0, 0)
        root.setColumnStretch(1, 1)
        root.setColumnStretch(2, 1)
        root.setRowStretch(0, 1)

        left = QtWidgets.QVBoxLayout()
        left.addWidget(self._group_serial())
        left.addWidget(self._group_time())
        left.addWidget(self._group_display())
        left.addStretch(1)
        root.addLayout(left, 0, 0)

        center = QtWidgets.QVBoxLayout()
        self.twin = TwinPanel()
        center.addWidget(self._wrap("Digital Twin", self.twin))
        center.addWidget(self._group_extensions())
        root.addLayout(center, 0, 1)

        right = QtWidgets.QVBoxLayout()
        right.addWidget(self._group_manual())
        right.addWidget(self._group_acceptance())
        right.addWidget(self._group_log(), 1)
        root.addLayout(right, 0, 2)

    def _wrap(self, title: str, widget: QtWidgets.QWidget) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox(title)
        layout = QtWidgets.QVBoxLayout(group)
        layout.addWidget(widget)
        return group

    def _group_serial(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Serial")
        layout = QtWidgets.QGridLayout(group)
        self.port_box = QtWidgets.QComboBox()
        self.refresh_button = QtWidgets.QPushButton("Refresh")
        self.connect_button = QtWidgets.QPushButton("Connect")
        self.disconnect_button = QtWidgets.QPushButton("Disconnect")
        self.disconnect_button.setEnabled(False)
        self.latency_label = QtWidgets.QLabel("Latency -- ms")
        layout.addWidget(self.port_box, 0, 0, 1, 2)
        layout.addWidget(self.refresh_button, 0, 2)
        layout.addWidget(self.connect_button, 1, 0)
        layout.addWidget(self.disconnect_button, 1, 1)
        layout.addWidget(self.latency_label, 1, 2)
        return group

    def _group_time(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Clock")
        layout = QtWidgets.QGridLayout(group)
        now = datetime.now()
        self.date_edit = QtWidgets.QDateEdit(QtCore.QDate(now.year, now.month, now.day))
        self.date_edit.setCalendarPopup(True)
        self.time_edit = QtWidgets.QTimeEdit(QtCore.QTime(now.hour, now.minute, now.second))
        self.alarm_edit = QtWidgets.QTimeEdit(QtCore.QTime(7, 0, 0))
        buttons = [
            ("Set Date", self.send_date),
            ("Set Time", self.send_time),
            ("Set Alarm", self.send_alarm),
            ("Alarm Off", lambda: self.send_command("*SET:ALARM OFF")),
            ("Get Time", lambda: self.send_command("*GET:TIME")),
            ("Ping", lambda: self.send_command("*PING")),
        ]
        layout.addWidget(QtWidgets.QLabel("Date"), 0, 0)
        layout.addWidget(self.date_edit, 0, 1, 1, 2)
        layout.addWidget(QtWidgets.QLabel("Time"), 1, 0)
        layout.addWidget(self.time_edit, 1, 1, 1, 2)
        layout.addWidget(QtWidgets.QLabel("Alarm"), 2, 0)
        layout.addWidget(self.alarm_edit, 2, 1, 1, 2)
        for i, (text, slot) in enumerate(buttons):
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(slot)
            layout.addWidget(button, 3 + i // 3, i % 3)
        return group

    def _group_display(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Display Control")
        layout = QtWidgets.QGridLayout(group)
        self.message_edit = QtWidgets.QLineEdit("HELLO S800 CLOCK")
        self.led_edit = QtWidgets.QLineEdit("3F")
        self.beep_spin = QtWidgets.QSpinBox()
        self.beep_spin.setRange(10, 5000)
        self.beep_spin.setValue(500)
        items = [
            ("Msg", self.message_edit, lambda: self.send_command(f"*SET:MSG {self.message_edit.text()}")),
            ("LED", self.led_edit, lambda: self.send_command(f"*SET:LED {self.led_edit.text()}")),
            ("Beep ms", self.beep_spin, lambda: self.send_command(f"*SET:BEEP {self.beep_spin.value()}")),
        ]
        for row, (label, widget, slot) in enumerate(items):
            layout.addWidget(QtWidgets.QLabel(label), row, 0)
            layout.addWidget(widget, row, 1)
            button = QtWidgets.QPushButton("Send")
            button.clicked.connect(slot)
            layout.addWidget(button, row, 2)
        for i, (text, command) in enumerate(
            [
                ("DISP ON", "*SET:DISP ON"),
                ("DISP OFF", "*SET:DISP OFF"),
                ("LEFT", "*SET:FORMAT LEFT"),
                ("RIGHT", "*SET:FORMAT RIGHT"),
                ("LED AUTO", "*SET:LED 00"),
                ("RST", "*RST"),
            ]
        ):
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(lambda checked=False, c=command: self.send_command(c))
            layout.addWidget(button, 3 + i // 3, i % 3)
        return group

    def _group_extensions(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Extensions")
        layout = QtWidgets.QGridLayout(group)
        self.weather_location = QtWidgets.QLineEdit("")
        self.weather_location.setPlaceholderText("wttr.in location, blank = auto")
        buttons = [
            ("NTP Sync", self.sync_ntp),
            ("Fetch Weather", self.fetch_weather),
            ("Send Weather", self.send_cached_weather),
            ("DAY", lambda: self.send_command("*SET:MODE DAY")),
            ("NIGHT", lambda: self.send_command("*SET:MODE NIGHT")),
            ("Auto Day/Night", self.auto_day_night),
            ("EVT ON", lambda: self.send_command("*SET EVT=ON")),
            ("EVT OFF", lambda: self.send_command("*SET EVT=OFF")),
            ("Charts", self.show_charts),
        ]
        layout.addWidget(self.weather_location, 0, 0, 1, 4)
        for i, (text, slot) in enumerate(buttons):
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(slot)
            layout.addWidget(button, 1 + i // 4, i % 4)
        return group

    def _group_manual(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Manual Command")
        layout = QtWidgets.QHBoxLayout(group)
        self.command_edit = QtWidgets.QLineEdit("*GET:TIME")
        self.send_button = QtWidgets.QPushButton("Send")
        layout.addWidget(self.command_edit, 1)
        layout.addWidget(self.send_button)
        return group

    def _group_acceptance(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Acceptance")
        layout = QtWidgets.QGridLayout(group)
        self.run_accept_button = QtWidgets.QPushButton("Run Smoke Test")
        self.export_log_button = QtWidgets.QPushButton("Export Log")
        self.test_label = QtWidgets.QLabel("Ready")
        layout.addWidget(self.run_accept_button, 0, 0)
        layout.addWidget(self.export_log_button, 0, 1)
        layout.addWidget(self.test_label, 1, 0, 1, 2)
        return group

    def _group_log(self) -> QtWidgets.QGroupBox:
        group = QtWidgets.QGroupBox("Log")
        layout = QtWidgets.QVBoxLayout(group)
        self.log_view = QtWidgets.QTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setFont(QtGui.QFont("Consolas", 10))
        layout.addWidget(self.log_view)
        return group

    def _connect_signals(self) -> None:
        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.connect_serial)
        self.disconnect_button.clicked.connect(self.worker.close_port)
        self.send_button.clicked.connect(lambda: self.send_command(self.command_edit.text()))
        self.command_edit.returnPressed.connect(lambda: self.send_command(self.command_edit.text()))
        self.twin.key_clicked.connect(lambda name: self.send_command(f"*SET:KEY {name}"))
        self.worker.line_received.connect(self.on_line_received)
        self.worker.connection_changed.connect(self.on_connection_changed)
        self.worker.latency_updated.connect(lambda ms: self.latency_label.setText(f"Latency {ms} ms"))
        self.worker.error_occurred.connect(self.on_error)
        self.run_accept_button.clicked.connect(self.run_smoke_test)
        self.export_log_button.clicked.connect(self.export_log)

    def refresh_ports(self) -> None:
        current = self.port_box.currentText()
        self.port_box.clear()
        ports = SerialWorker.scan_ports()
        self.port_box.addItems(ports)
        if current in ports:
            self.port_box.setCurrentText(current)

    def connect_serial(self) -> None:
        port = self.port_box.currentText().strip()
        if not port:
            self.on_error("no serial port selected")
            return
        self.worker.open_port(port)

    def on_connection_changed(self, connected: bool, port: str) -> None:
        self.connect_button.setEnabled(not connected)
        self.disconnect_button.setEnabled(connected)
        self.statusBar().showMessage(f"{'Connected' if connected else 'Disconnected'} {port}")
        self.log("RX", f"{'connected' if connected else 'disconnected'} {port}", LOG_RX)

    def send_command(self, command: str) -> None:
        command = command.strip()
        if not command:
            return
        command = self.normalize_manual_command(command)
        if self.command_edit.text().strip() != command:
            self.command_edit.setText(command)
        self.log("TX", command, LOG_TX)
        self.worker.send_line(command)

    @staticmethod
    def normalize_manual_command(command: str) -> str:
        upper = command.upper()
        known_prefixes = (
            "*", "SET", "GET", "TIME", "DATE", "DISP", "MSG", "AT+", "RST", "NTP"
        )
        if upper.startswith(known_prefixes):
            return command
        return f"*SET:MSG {command}"

    def enqueue_commands(self, commands: list[str], gap_ms: int = 450) -> None:
        if not commands:
            return
        if self.queue_active:
            self.command_queue.extend(commands)
            return
        self.command_queue = list(commands)
        self.queue_active = True
        self._send_next_queued(gap_ms)

    def _send_next_queued(self, gap_ms: int = 450) -> None:
        if not self.command_queue:
            self.queue_active = False
            return
        command = self.command_queue.pop(0)
        self.send_command(command)
        QtCore.QTimer.singleShot(gap_ms, lambda: self._send_next_queued(gap_ms))

    def send_date(self) -> None:
        date = self.date_edit.date()
        self.send_command(f"*SET:DATE YEAR MONTH DATE {date.year():04d} {date.month():02d} {date.day():02d}")

    def send_time(self) -> None:
        time = self.time_edit.time()
        self.send_command(f"*SET:TIME HOUR MIN SEC {time.hour():02d} {time.minute():02d} {time.second():02d}")

    def send_alarm(self) -> None:
        time = self.alarm_edit.time()
        self.send_command(f"*SET:ALARM HOUR MIN SEC {time.hour():02d} {time.minute():02d} {time.second():02d}")

    def sync_ntp(self) -> None:
        try:
            dt = get_ntp_time()
        except Exception as exc:  # noqa: BLE001
            self.on_error(f"NTP failed: {exc}")
            return
        self.enqueue_commands(commands_for_datetime(dt), gap_ms=650)

    def fetch_weather(self) -> None:
        try:
            self.last_weather = get_weather(self.weather_location.text())
        except Exception as exc:  # noqa: BLE001
            self.on_error(f"weather failed: {exc}")
            return
        self.log(
            "RX",
            f"weather {self.last_weather.temperature_c}C {self.last_weather.condition} ({self.last_weather.source_text})",
            LOG_RX,
        )
        self.send_cached_weather()

    def send_cached_weather(self) -> None:
        if self.last_weather is None:
            temp = 31
            cond = "SUN"
            self.last_weather = WeatherData(temp, cond, "manual fallback")
        self.enqueue_commands(commands_for_weather(self.last_weather), gap_ms=500)

    def auto_day_night(self) -> None:
        city = LocationInfo("Shanghai", "China", "Asia/Shanghai", 31.2304, 121.4737)
        now = datetime.now(city.tzinfo)
        daylight = sun(city.observer, date=now.date(), tzinfo=city.timezone)
        mode = "DAY" if daylight["sunrise"] <= now <= daylight["sunset"] else "NIGHT"
        self.send_command(f"*SET:MODE {mode}")
        self.log("EVT", f"auto day/night selected {mode}", LOG_EVT)

    def show_charts(self) -> None:
        if not EVENT_CSV.exists():
            self.on_error("events.csv does not exist yet")
            return
        counts: dict[str, int] = {}
        with EVENT_CSV.open("r", newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                kind = row.get("kind", "UNKNOWN")
                counts[kind] = counts.get(kind, 0) + 1
        if not counts:
            self.on_error("events.csv has no events")
            return
        plt.figure(figsize=(7, 4))
        plt.bar(list(counts.keys()), list(counts.values()), color="#2d7dd2")
        plt.title("S800 Event Counts")
        plt.xlabel("Event")
        plt.ylabel("Count")
        plt.tight_layout()
        chart_path = APP_DIR / "events_chart.png"
        plt.savefig(chart_path, dpi=140)
        plt.close()
        QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(str(chart_path)))
        self.log("EVT", f"chart exported to {chart_path}", LOG_EVT)

    def run_smoke_test(self) -> None:
        self.pending_tests = [
            ("PING", "*PING", False),
            ("SET DATE", "*SET:DATE YEAR MONTH DATE 2026 06 10", False),
            ("SET TIME", "*SET:TIME HOUR MIN SEC 12 30 45", False),
            ("GET TIME", "*GET:TIME", False),
            ("FORMAT RIGHT", "*SET:FORMAT RIGHT", False),
            ("GET TIME RIGHT", "*GET:TIME", False),
            ("DISP OFF", "*SET:DISP OFF", False),
            ("DISP ON", "*SET:DISP ON", False),
            ("LED", "*SET:LED FF", False),
            ("LED AUTO", "*SET:LED 00", False),
            ("BEEP", "*SET:BEEP 500", False),
            ("KEY DISP", "*SET:KEY DISP", False),
            ("BEEP RANGE ERROR", "*SET:BEEP 9999", True),
        ]
        self.test_results = []
        self.current_test = None
        self.test_label.setText("Running smoke test...")
        self._run_next_test()

    def _run_next_test(self) -> None:
        if not self.pending_tests:
            passed = sum(1 for _, ok, _ in self.test_results if ok)
            total = len(self.test_results)
            self.test_label.setText(f"Passed {passed}/{total}")
            self.log("EVT", f"SMOKE TEST Passed {passed}/{total}", LOG_EVT)
            self.current_test = None
            return
        name, command, expect_error = self.pending_tests.pop(0)
        self.current_test = (name, command, expect_error)
        self.test_serial += 1
        serial = self.test_serial
        self.send_command(command)
        QtCore.QTimer.singleShot(1200, lambda s=serial: self._test_timeout(s))

    def _test_timeout(self, serial: int) -> None:
        if serial != self.test_serial:
            return
        if self.current_test is None:
            return
        name, _, _ = self.current_test
        self.test_results.append((name, False, "timeout"))
        self.current_test = None
        self._run_next_test()

    def on_line_received(self, line: str) -> None:
        parsed = parse_line(line)
        if isinstance(parsed, DispEvent):
            self.twin.update_display(parsed.chars, parsed.dp_mask)
            self.log("EVT", line, LOG_EVT)
        elif isinstance(parsed, LedEvent):
            self.twin.update_leds(parsed.value)
            self.log("EVT", line, LOG_EVT)
        elif isinstance(parsed, KeyEvent):
            self.twin.highlight_key(parsed.name)
            self.log("EVT", line, LOG_EVT)
            if parsed.name == "USER1":
                self.sync_ntp()
            self._record_event("KEY", parsed.name)
        elif isinstance(parsed, AlarmEvent):
            self.log("EVT", line, LOG_EVT)
            self._record_event("ALARM", "ON" if parsed.active else "OFF")
        elif isinstance(parsed, EditEvent):
            self.log("EVT", line, LOG_EVT)
            self._record_event("EDIT", f"{parsed.kind} {parsed.value}")
        elif isinstance(parsed, ModeEvent):
            self.log("EVT", line, LOG_EVT)
            self.statusBar().showMessage(f"MODE {parsed.mode}")
        elif isinstance(parsed, PongEvent):
            self.log("RX", line, LOG_RX)
            self._mark_latest_test(True, line)
        elif isinstance(parsed, OkResponse):
            self.log("RX", line, LOG_RX)
            self._mark_latest_test(True, line)
        elif isinstance(parsed, ErrorResponse):
            self.log("ERR", line, LOG_ERR)
            self._mark_latest_test(False, line)
        elif isinstance(parsed, RxEcho):
            self.log("RX", line, LOG_RX)
        elif isinstance(parsed, UnknownLine):
            self.log("RX", line, LOG_RX)
            self._mark_get_response_if_expected(line)

    def _mark_get_response_if_expected(self, line: str) -> None:
        if self.current_test is None:
            return
        _, command, expect_error = self.current_test
        if expect_error:
            return
        upper_command = command.upper()
        if not upper_command.startswith("*GET:"):
            return
        topic = upper_command.split(":", 1)[1].split()[0]
        if line.upper().startswith(f"{topic} "):
            self._mark_latest_test(True, line)

    def _mark_latest_test(self, ok: bool, detail: str) -> None:
        if self.current_test is None:
            return
        name, _, expect_error = self.current_test
        passed = (ok and not expect_error) or ((not ok) and expect_error)
        if expect_error and "ERROR" in detail.upper():
            passed = True
        self.test_results.append((name, passed, detail))
        self.current_test = None
        QtCore.QTimer.singleShot(250, self._run_next_test)

    def _record_event(self, kind: str, detail: str) -> None:
        first_write = not EVENT_CSV.exists()
        with EVENT_CSV.open("a", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            if first_write:
                writer.writerow(["timestamp", "kind", "detail"])
            writer.writerow([datetime.now().isoformat(timespec="seconds"), kind, detail])

    def on_error(self, message: str) -> None:
        self.log("ERR", message, LOG_ERR)
        QtWidgets.QMessageBox.warning(self, "Error", message)

    def log(self, tag: str, text: str, color: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        escaped = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        self.log_view.append(f'<span style="color:{color}">[{stamp}] {tag:<3} {escaped}</span>')

    def export_log(self) -> None:
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "Export Log", str(APP_DIR / "pc_host_log.html"), "HTML (*.html)")
        if path:
            Path(path).write_text(self.log_view.toHtml(), encoding="utf-8")
            self.log("RX", f"exported log to {path}", LOG_RX)

    def closeEvent(self, event):  # noqa: N802
        self.worker.stop()
        super().closeEvent(event)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle("Fusion")
    window = MainWindow()
    window.show()
    return app.exec_()


if __name__ == "__main__":
    raise SystemExit(main())
