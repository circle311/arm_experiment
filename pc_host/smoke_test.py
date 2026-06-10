from __future__ import annotations

from protocol import (
    AlarmEvent,
    DispEvent,
    ErrorResponse,
    KeyEvent,
    LedEvent,
    ModeEvent,
    OkResponse,
    PongEvent,
    led_bits,
    parse_line,
)


def assert_type(line: str, expected_type: type) -> object:
    parsed = parse_line(line)
    assert isinstance(parsed, expected_type), f"{line!r} parsed as {parsed!r}"
    return parsed


def main() -> int:
    disp = assert_type("*EVT:DISP 123045__ 0A", DispEvent)
    assert disp.chars == "123045  "
    assert disp.dp_mask == 0x0A

    led = assert_type("*EVT:LED 3F", LedEvent)
    assert led.value == 0x3F
    assert led_bits(led.value)[:6] == [True, True, True, True, True, True]
    assert led_bits(led.value)[6:] == [False, False]

    key = assert_type("*EVT:KEY USER1", KeyEvent)
    assert key.name == "USER1"

    assert_type("*EVT:ALARM", AlarmEvent)
    assert_type("*EVT:MODE NIGHT", ModeEvent)
    assert_type("*PONG 42", PongEvent)
    assert_type("OK TIME", OkResponse)
    assert_type("ERROR RANGE", ErrorResponse)
    print("protocol smoke test OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
