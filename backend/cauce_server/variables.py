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
    #: Spanish label. The dashboard serves three languages and the legend is read by a person,
    #: so an English-only name on a Spanish page is a regression rather than a shortcut - which
    #: is what happened when the legend started reading from this table.
    label_es: str = ""
    #: Variables that describe the atmosphere itself rather than the hardware or its placement.
    environmental: bool = True
    #: The range the map's colour ramp covers for this channel. A ramp has to be pinned to
    #: something: temperature's 0..40 C is a choice, and reusing it for wind or VOC would make
    #: every reading of those collapse onto one end of the gradient and read as "extreme" when
    #: it is ordinary. These are the full-scale-ish ranges the parts actually work in, so the
    #: ramp spreads a day's readings across it.
    scale_lo: float = 0.0
    scale_hi: float = 100.0


# Order matters: it is the order the legend and the default report selection use, so
# air_temperature stays first and nothing a reader has learned to look for moves.
VARIABLES: tuple[VariableInfo, ...] = (
    VariableInfo("air_temperature", "°C", "Air temperature", "#39c2a7",
                 label_es="Temperatura del aire", scale_hi=40.0),
    VariableInfo("relative_humidity", "%RH", "Relative humidity", "#7cc4ff",
                 label_es="Humedad relativa", scale_hi=100.0),
    VariableInfo("pressure", "hPa", "Pressure", "#e6b455",
                 label_es="Presión", scale_lo=980.0, scale_hi=1040.0),
    # Present in the data model since before any driver produced it; the driver arrived later.
    VariableInfo("illuminance", "lx", "Illuminance", "#f2d16b",
                 label_es="Iluminancia", scale_hi=20000.0),
    # Total VOC. Deliberately not CO2 - see the CCS811 driver for why that number is not
    # reported. If it ever appears here, it is being read as a measurement it is not.
    VariableInfo("tvoc", "ppb", "Total VOC", "#c98fe0",
                 label_es="VOC total", scale_hi=1000.0),
    VariableInfo("wind_speed", "m/s", "Wind speed", "#6fd3c7",
                 label_es="Velocidad del viento", scale_hi=25.0),
    VariableInfo("battery_voltage", "V", "Battery voltage", "#9aa0a6",
                 label_es="Tensión de batería", environmental=False,
                 scale_lo=3.0, scale_hi=4.2),
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


def label_for(variable: str, lang: str = "es") -> str:
    info = BY_NAME.get(variable)
    if not info:
        return (variable or "").replace("_", " ")
    if lang and lang.lower().startswith("es") and info.label_es:
        return info.label_es
    return info.label


def scale_for(variable: str) -> tuple[float, float]:
    """The colour ramp's range for a channel, falling back to temperature's."""
    info = BY_NAME.get(variable)
    return (info.scale_lo, info.scale_hi) if info else (0.0, 40.0)


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


def resolve(available: list[str] | None) -> str | None:
    """The one channel to use when a caller names a single variable.

    Several pages - the node events page, the heat summary, the alert rule evaluation - take a
    variable parameter whose default is `air_temperature`. A default is the right shape for an
    API, but a *hardcoded* default leaks into the query when the node has no temperature, and
    the result is an empty page on a node that is reporting perfectly well. This is the
    fallback those call sites share: temperature when the node has it, otherwise the first
    environmental channel it does have, otherwise the first of anything.

    Returns None for no data at all, so the caller can tell "nothing to show" from "show
    temperature" - a distinction a bare default cannot express.
    """
    if not available:
        return None
    if "air_temperature" in available:
        return "air_temperature"
    order = [v.name for v in VARIABLES]
    environmental = [v for v in available if is_environmental(v) and v in order]
    if environmental:
        return min(environmental, key=order.index)
    return available[0]


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
