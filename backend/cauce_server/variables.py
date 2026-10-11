"""What a measurement variable means, in one place.

The storage layer is generic - a measurement is a `(node, variable, value, unit)` row and any
channel can be written to it without a schema change. Presentation was not: the map coloured
only `air_temperature`, the legend was fixed, and the node report preferred two hardcoded names.
Twenty-three references to `air_temperature` in `dashboard.py` meant a sensor could report
something real, be stored correctly, and be invisible - which is the same failure as a feature
that was never built, except it looks like it works.

So the presentation vocabulary lives here instead, and it is derived rather than duplicated.
`VARIABLES` is the single declaration of what exists; the legend, the default selection, the
colour and the display name all read from it. Adding a channel is now one entry rather than
four scattered edits that have to be found by hand.

The colour choices are not decoration. Temperature keeps the teal it has always had so existing
screenshots and the heat map do not change meaning under anyone; the new channels get colours
chosen to stay apart from it in the common low-light case a node actually sits in.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class VariableInfo:
    name: str
    unit: str
    label: str
    color: str
    #: Variables that describe the atmosphere itself rather than the hardware or its placement.
    environmental: bool = True


# Order matters: it is the order the legend and the default report selection use, so
# air_temperature stays first and nothing a reader has learned to look for moves.
VARIABLES: tuple[VariableInfo, ...] = (
    VariableInfo("air_temperature", "C", "Air temperature", "#39c2a7"),
    VariableInfo("relative_humidity", "%RH", "Relative humidity", "#7cc4ff"),
    VariableInfo("pressure", "hPa", "Pressure", "#e6b455"),
    # Present in the data model since before any driver produced it; the driver arrived later.
    VariableInfo("illuminance", "lx", "Illuminance", "#f2d16b"),
    # Total VOC. Deliberately not CO2 - see the CCS811 driver for why that number is not
    # reported. If it ever appears here, it is being read as a measurement it is not.
    VariableInfo("tvoc", "ppb", "Total VOC", "#c98fe0"),
    VariableInfo("wind_speed", "m/s", "Wind speed", "#6fd3c7"),
    VariableInfo("battery_voltage", "V", "Battery voltage", "#9aa0a6", environmental=False),
)

BY_NAME: dict[str, VariableInfo] = {v.name: v for v in VARIABLES}

#: Shown when a node reports nothing, so the reader knows the channel exists and is empty
#: rather than not existing at all.
PREFERRED_FIRST: tuple[str, ...] = ("air_temperature", "relative_humidity")

#: Fallback colour for a channel a node reports that predates this table. A variable the
#: database knows and this file does not still has to render.
FALLBACK_COLOR = "#b0b0b0"


def color_for(variable: str) -> str:
    info = BY_NAME.get(variable)
    return info.color if info else FALLBACK_COLOR


def label_for(variable: str) -> str:
    info = BY_NAME.get(variable)
    return info.label if info else (variable or "").replace("_", " ")


def unit_for(variable: str) -> str:
    info = BY_NAME.get(variable)
    return info.unit if info else ""


def is_environmental(variable: str) -> bool:
    info = BY_NAME.get(variable)
    return bool(info and info.environmental)


def default_selection(available: list[str]) -> list[str]:
    """Which channels a node report shows when the caller does not ask for specific ones.

    Falls back to everything the node actually reports rather than to a fixed list: a node
    carrying wind and nothing else should still show wind, not an empty report.
    """
    preferred = [v for v in PREFERRED_FIRST if v in available]
    return preferred or available


def legend(available: list[str]) -> list[dict[str, str]]:
    """The legend entries for the channels a node actually reports."""
    entries = []
    for info in VARIABLES:
        if info.name in available:
            entries.append({"variable": info.name, "label": info.label,
                            "unit": info.unit, "color": info.color})
    # Channels this table has never heard of still have to appear, or a node reporting one
    # renders a series with no legend entry explaining it.
    for name in available:
        if name not in BY_NAME:
            entries.append({"variable": name, "label": label_for(name),
                            "unit": unit_for(name), "color": FALLBACK_COLOR})
    return entries
