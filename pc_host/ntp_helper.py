from __future__ import annotations

from datetime import datetime

import ntplib


def get_ntp_time(server: str = "ntp.aliyun.com", timeout: float = 3.0) -> datetime:
    client = ntplib.NTPClient()
    response = client.request(server, version=3, timeout=timeout)
    return datetime.fromtimestamp(response.tx_time)


def commands_for_datetime(dt: datetime) -> list[str]:
    return [
        f"*SET:DATE YEAR MONTH DATE {dt.year:04d} {dt.month:02d} {dt.day:02d}",
        f"*SET:TIME HOUR MIN SEC {dt.hour:02d} {dt.minute:02d} {dt.second:02d}",
        "*NTP SYNC",
    ]
