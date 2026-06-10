from __future__ import annotations

from dataclasses import dataclass

import requests


@dataclass(frozen=True)
class WeatherData:
    temperature_c: int
    condition: str
    source_text: str


def normalize_condition(text: str) -> str:
    value = text.upper()
    if "SNOW" in value or "SNO" in value:
        return "SNO"
    if "RAIN" in value or "SHOWER" in value or "DRIZZLE" in value:
        return "RAI"
    if "FOG" in value or "MIST" in value or "HAZE" in value:
        return "FOG"
    if "OVERCAST" in value or "OVC" in value:
        return "OVC"
    if "CLOUD" in value or "CLD" in value:
        return "CLD"
    return "SUN"


def get_weather(location: str = "") -> WeatherData:
    query = location.strip() or "auto"
    url = f"https://wttr.in/{query}"
    response = requests.get(url, params={"format": "j1"}, timeout=5)
    response.raise_for_status()
    data = response.json()
    current = data["current_condition"][0]
    temp = int(current["temp_C"])
    desc = current["weatherDesc"][0]["value"]
    return WeatherData(temperature_c=temp, condition=normalize_condition(desc), source_text=desc)


def commands_for_weather(weather: WeatherData) -> list[str]:
    return [
        f"*SET:WEA {weather.temperature_c} {weather.condition}",
        f"*SET:MSG {weather.temperature_c:+03d}C {weather.condition}",
    ]
