from __future__ import annotations

import time
from typing import Optional

import serial
from serial.tools import list_ports
from PyQt5 import QtCore

from protocol import format_command


class SerialWorker(QtCore.QThread):
    line_received = QtCore.pyqtSignal(str)
    connection_changed = QtCore.pyqtSignal(bool, str)
    latency_updated = QtCore.pyqtSignal(int)
    error_occurred = QtCore.pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._serial: Optional[serial.Serial] = None
        self._running = False
        self._last_ping_sent: Optional[float] = None
        self._port = ""
        self._tx_queue: list[str] = []
        self._last_tx_at = 0.0
        self._tx_gap_s = 0.5
        self._tx_timer = QtCore.QTimer(self)
        self._tx_timer.timeout.connect(self._drain_tx_queue)

    @staticmethod
    def scan_ports() -> list[str]:
        return [port.device for port in list_ports.comports()]

    def open_port(self, port: str, baud: int = 115200) -> None:
        self.close_port()
        try:
            self._serial = serial.Serial(port=port, baudrate=baud, timeout=0.05)
            self._port = port
            self._running = True
            if not self.isRunning():
                self.start()
            self.connection_changed.emit(True, port)
        except serial.SerialException as exc:
            self._serial = None
            self.error_occurred.emit(str(exc))
            self.connection_changed.emit(False, port)

    def close_port(self) -> None:
        self._running = False
        self._tx_queue.clear()
        self._tx_timer.stop()
        if self._serial is not None:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        self._serial = None
        if self._port:
            self.connection_changed.emit(False, self._port)

    def send_line(self, command: str) -> None:
        if self._serial is None or not self._serial.is_open:
            self.error_occurred.emit("serial port is not open")
            return
        data = format_command(command)
        if not data:
            return
        self._tx_queue.append(command)
        if not self._tx_timer.isActive():
            self._schedule_tx()

    def _schedule_tx(self) -> None:
        elapsed = time.monotonic() - self._last_tx_at
        delay_s = max(0.0, self._tx_gap_s - elapsed)
        self._tx_timer.start(max(1, int(delay_s * 1000)))

    def _drain_tx_queue(self) -> None:
        if self._serial is None or not self._serial.is_open:
            self._tx_queue.clear()
            self._tx_timer.stop()
            return
        if not self._tx_queue:
            self._tx_timer.stop()
            return

        elapsed = time.monotonic() - self._last_tx_at
        if elapsed < self._tx_gap_s:
            self._schedule_tx()
            return

        command = self._tx_queue.pop(0)
        data = format_command(command)
        if command.strip().upper() == "*PING":
            self._last_ping_sent = time.monotonic()
        try:
            self._serial.write(data.encode("ascii", errors="ignore"))
            self._last_tx_at = time.monotonic()
        except serial.SerialException as exc:
            self.error_occurred.emit(str(exc))
        if not self._tx_queue:
            self._tx_timer.stop()
        else:
            self._schedule_tx()

    def run(self) -> None:
        buffer = bytearray()
        while self._running:
            ser = self._serial
            if ser is None or not ser.is_open:
                self.msleep(50)
                continue
            try:
                chunk = ser.read(128)
            except serial.SerialException as exc:
                self.error_occurred.emit(str(exc))
                self.close_port()
                break
            if not chunk:
                continue
            for byte in chunk:
                if byte in (10, 13):
                    if buffer:
                        line = buffer.decode("ascii", errors="replace").strip()
                        buffer.clear()
                        if line:
                            if line.upper().startswith("*PONG") and self._last_ping_sent is not None:
                                latency_ms = int((time.monotonic() - self._last_ping_sent) * 1000)
                                self.latency_updated.emit(latency_ms)
                                self._last_ping_sent = None
                            self.line_received.emit(line)
                else:
                    buffer.append(byte)

    def stop(self) -> None:
        self.close_port()
        self.wait(1000)
