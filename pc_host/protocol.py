from __future__ import annotations

from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class DispEvent:
    chars: str
    dp_mask: int


@dataclass(frozen=True)
class LedEvent:
    value: int


@dataclass(frozen=True)
class KeyEvent:
    name: str


@dataclass(frozen=True)
class AlarmEvent:
    active: bool


@dataclass(frozen=True)
class EditEvent:
    kind: str
    value: str


@dataclass(frozen=True)
class ModeEvent:
    mode: str


@dataclass(frozen=True)
class PongEvent:
    uptime_s: int


@dataclass(frozen=True)
class OkResponse:
    text: str


@dataclass(frozen=True)
class ErrorResponse:
    reason: str


@dataclass(frozen=True)
class RxEcho:
    text: str


@dataclass(frozen=True)
class UnknownLine:
    text: str


ParsedLine = (
    DispEvent
    | LedEvent
    | KeyEvent
    | AlarmEvent
    | EditEvent
    | ModeEvent
    | PongEvent
    | OkResponse
    | ErrorResponse
    | RxEcho
    | UnknownLine
)


def parse_line(line: str) -> ParsedLine:
    text = line.strip()
    if not text:
        return UnknownLine(text="")

    upper = text.upper()
    parts = text.split()

    if upper.startswith("RX:"):
        return RxEcho(text=text[3:].strip())

    if len(parts) >= 3 and parts[0].upper() == "*EVT:DISP":
        chars = parts[1]
        if len(chars) == 8:
            try:
                return DispEvent(chars=chars.replace("_", " "), dp_mask=int(parts[2], 16))
            except ValueError:
                pass

    if len(parts) >= 2 and parts[0].upper() == "*EVT:LED":
        try:
            return LedEvent(value=int(parts[1].removeprefix("0x").removeprefix("0X"), 16))
        except ValueError:
            pass

    if len(parts) >= 2 and parts[0].upper() == "*EVT:KEY":
        return KeyEvent(name=parts[1].upper())

    if parts[0].upper() == "*EVT:ALARM":
        return AlarmEvent(active=True)

    if parts[0].upper() == "*EVT:ALARM_OFF":
        return AlarmEvent(active=False)

    if len(parts) >= 3 and parts[0].upper() == "*EVT:EDIT":
        return EditEvent(kind=parts[1].upper(), value=" ".join(parts[2:]))

    if len(parts) >= 2 and parts[0].upper() == "*EVT:MODE":
        return ModeEvent(mode=parts[1].upper())

    if len(parts) >= 2 and parts[0].upper() == "*PONG":
        try:
            return PongEvent(uptime_s=int(parts[1]))
        except ValueError:
            pass

    if parts[0].upper() == "OK":
        return OkResponse(text=text)

    if parts[0].upper() == "ERROR":
        return ErrorResponse(reason=" ".join(parts[1:]) or "UNKNOWN")

    return UnknownLine(text=text)


def led_bits(value: int) -> list[bool]:
    return [bool(value & (1 << i)) for i in range(8)]


def format_command(command: str) -> str:
    command = command.strip()
    if not command:
        return ""
    return command if command.endswith(("\r", "\n")) else command + "\r\n"


def is_error(line: str) -> bool:
    return isinstance(parse_line(line), ErrorResponse)


def expected_ok(line: str) -> Optional[str]:
    parsed = parse_line(line)
    if isinstance(parsed, OkResponse):
        return parsed.text
    if isinstance(parsed, PongEvent):
        return f"*PONG {parsed.uptime_s}"
    return None
