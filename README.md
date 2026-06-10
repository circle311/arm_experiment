# S800 Smart Network Clock

This repository contains the MCU firmware and the Python/PyQt5 PC host for the S800 smart clock coursework.

## Current Status

- MCU firmware: `ARM/exp3-1.c`
- Keil project: `ARM/exp3.uvprojx`
- PC host: `pc_host/main.py`
- Work log: `工作日志.md`
- Latest requirement alignment: `docs/新资料对照与第三周计划.md`

## MCU Build

Open `ARM/exp3.uvprojx` in Keil, build, and download to the board.

Useful SSCOM checks:

```text
*PING
*SET:TIME HOUR MIN SEC 19 56 00
*SET:BEEP 500
*SET:LED 3F
*SET:LED 00
*SET:MODE NIGHT
*SET:MODE DAY
*SET:WEA 31 SUN
*NTP SYNC
```

Expected highlights:

- `*PING` returns `*PONG <uptime_s>`.
- `*SET:BEEP 500` beeps for about 0.5 s.
- `*SET:LED 3F` lights LED0-LED5 and enters LED override.
- `*SET:LED 00` exits LED override.
- `*SET:MODE NIGHT` shows only HH.MM and keeps heartbeat LED.
- `*SET:MODE DAY` restores normal display.
- `*SET:WEA 31 SUN` stores weather and updates weather LEDs.
- `*NTP SYNC` enables the NTP status LED.

## PC Host

Always verify the virtual environment before PC development or running:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pc_host\verify_env.ps1
```

Run the PC host:

```powershell
.\pc_host\.venv\Scripts\python.exe .\pc_host\main.py
```

Run protocol smoke tests:

```powershell
.\pc_host\.venv\Scripts\python.exe .\pc_host\smoke_test.py
```

Expected smoke test output:

```text
protocol smoke test OK
```

## PC Host Features

- Serial scan/connect/disconnect at `115200 8N1`.
- Background serial receive thread.
- Digital twin panel:
  - 8 seven-segment digits with decimal points.
  - 8 LED indicators.
  - 8 normal keys plus USER1/USER2.
- Visual command panel for date, time, alarm, display, format, message, beep, LED, mode, weather, NTP.
- Event log with TX/RX/EVT/ERR coloring.
- USER1 event triggers NTP sync.
- Weather helper uses `wttr.in` and sends `*SET:WEA`.
- Auto day/night uses `astral` with Shanghai coordinates.
- Event CSV is written to `pc_host/events.csv`.
- Chart export creates `pc_host/events_chart.png`.
- Smoke-test panel sends a protocol regression sequence.

## Evaluation Flow

1. Build and download MCU firmware in Keil.
2. Open PC host.
3. Select the board COM port and click `Connect`.
4. Confirm 1 Hz mirror updates:
   - Log shows `*EVT:DISP <8chars> <dpHex>`.
   - Log shows `*EVT:LED <hex2>`.
   - Digital twin display and LEDs update.
5. Click virtual keys in the PC host:
   - Board reacts as if physical keys were pressed.
   - MCU should not echo `*EVT:KEY` for PC-originated `*SET:KEY`.
6. Press physical keys on the board:
   - PC log receives `*EVT:KEY <NAME>`.
   - Matching virtual key highlights briefly.
7. Run `Run Smoke Test`:
   - Expected result is mostly or fully passing.
   - Board should beep, display switch, LED override, and accept parameterized commands.
8. Test extensions:
   - `NTP Sync` sets board time and sends `*NTP SYNC`.
   - `Fetch Weather` sends weather commands.
   - `Auto Day/Night` sends `*SET:MODE DAY` or `*SET:MODE NIGHT`.
   - `Charts` opens an event-count chart after events have been logged.

## Display Ghosting Note

The MCU scan routine now blanks all digit selects before changing segment data and enabling the next digit. If very faint residual glow remains, it is likely hardware persistence/driver leakage; the current code-side mitigation is already the low-risk fix.
