"""Uncertainty budgets per quantity.

The roadmap item was "the budget is plumbed, it needs filling in", and that is
exactly what this is: the components, the combination rule, and numbers.

WHY A BUDGET RATHER THAN A SINGLE FIGURE

A datasheet tolerance, a co-location spread and an instrument's own repeatability
are different claims about different failure modes, and averaging them into one
number destroys the only information an operator needs. Someone deciding whether a
reading can be used for a heat-alert threshold needs to know whether the figure is
dominated by the sensor or by the comparison, because those call for different
responses: a bad sensor gets replaced, a bad comparison gets re-done.

So the components are kept, combined by the usual root-sum-of-squares because they
are independent, and the largest is reported so a single dominant term is visible
rather than buried.

WHAT THESE NUMBERS ARE NOT

They are a defensible starting budget for a BME280-class sensor installed per
docs/en/CALIBRATION.md, not a substitute for establishing traceability. They are
stated as assumptions, they name what would invalidate them, and
`budget_for` refuses to invent a budget for a variable it has no numbers for rather
than quietly returning a plausible-looking zero.

The figures are the kind that appear in a sensor datasheet as a typical or maximum
tolerance. They are recorded here as the project's stated assumption, with the
source named, so that a later metrological traceable calibration replaces a number
rather than rewriting a rationale.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

# The variables this project actually measures. A variable absent from here has no
# budget, and `budget_for` returns None for it - which is a different answer from
# zero, and the difference matters.
BUDGETS: dict[str, dict[str, dict]] = {
    "air_temperature": {
        "unit": "degC",
        "components": {
            # BME280 temperature accuracy, typical over the -40..85 C range.
            "sensor_datasheet": 0.5,
            # Spread across a co-located reference set, which is what turns a
            # single-sensor figure into a site figure.
            "co_location_spread": 0.3,
            # Half the resolution of the reported value: rounding is a real term
            # and omitting it is the most common way a budget understates.
            "quantisation": 0.01,
        },
        "assumption": (
            "A single BME280 compared against a co-located reference at 30 C, not "
            "traceable to a national standard. Valid until a traceable comparison "
            "exists, and the co-location term dominates the error budget, so the "
            "sensor is not the limiting factor."
        ),
    },
    "relative_humidity": {
        "unit": "%RH",
        "components": {
            # BME280 humidity accuracy is specified as a percentage of the reading,
            # so it is modelled as a proportional term below rather than a constant.
            "sensor_datasheet_pct_of_reading": 3.0,
            "co_location_spread": 2.0,
            "quantisation": 0.1,
        },
        "assumption": (
            "Humidity is the weaker quantity: the datasheet term is proportional, so "
            "the figure is not a constant and `combined_for_reading` returns one per "
            "reading. Co-location dominates again."
        ),
    },
    "pressure": {
        "unit": "hPa",
        "components": {
            # BME280 pressure accuracy, +/-1 hPa, which is already stated as an
            # absolute figure so it needs no reading-dependent treatment.
            "sensor_datasheet": 1.0,
            # Barometric readings also carry an altitude offset that no comparison
            # against a reference removes, which is why it is its own term.
            "altitude_offset": 0.0,
            "quantisation": 0.01,
        },
        "assumption": (
            "A relative pressure reading. Comparing against a station that reports "
            "sea-level pressure will disagree by roughly 1 hPa per 8.5 m of altitude "
            "and no calibration here can fix that."
        ),
    },
}

# Any component whose name ends in this suffix is proportional to the reading.
PROPORTIONAL_SUFFIX = "_pct_of_reading"


@dataclass(frozen=True)
class Budget:
    variable: str
    unit: str
    components: dict[str, float]
    assumption: str

    @property
    def has_proportional_terms(self) -> bool:
        return any(name.endswith(PROPORTIONAL_SUFFIX)
                   for name in self.components)

    def absolute_components(self) -> dict[str, float]:
        return {name: value for name, value in self.components.items()
                if not name.endswith(PROPORTIONAL_SUFFIX)}

    def combined_absolute(self) -> float:
        """Root-sum-of-squares over the constant terms. Returns 0.0, not None,
        for a budget with only proportional terms, because that is a meaningful
        zero and the proportional terms are handled elsewhere."""
        squares = [v * v for v in self.absolute_components().values()]
        return math.sqrt(sum(squares)) if squares else 0.0

    def combined_for_reading(self, reading: float | None) -> float:
        """Total uncertainty for one reading.

        Proportional terms are fractions of the reading, so the budget cannot be a
        single number. Callers that want a constant for display use
        `combined_absolute` and are then knowingly quoting the constant part only.
        """
        squares = [v * v for v in self.absolute_components().values()]
        for name, pct in self.components.items():
            if name.endswith(PROPORTIONAL_SUFFIX):
                value = reading if reading is not None else 0.0
                contribution = value * pct / 100.0
                squares.append(contribution * contribution)
        return math.sqrt(sum(squares)) if squares else 0.0

    def dominant_term(self, reading: float | None = None) -> str | None:
        """The largest single contribution, by name.

        Surfaced so a report can say which term to attack first, which is the whole
        reason for keeping the components rather than storing one number.
        """
        if not self.components:
            return None
        sizes: dict[str, float] = {}
        for name, pct in self.components.items():
            if name.endswith(PROPORTIONAL_SUFFIX):
                value = reading if reading is not None else 0.0
                sizes[name] = abs(value) * pct / 100.0
            else:
                sizes[name] = pct
        return max(sizes.items(), key=lambda kv: kv[1])[0]

    def as_dict(self, reading: float | None = None) -> dict:
        return {
            "variable": self.variable,
            "unit": self.unit,
            "components": dict(self.components),
            "combined_absolute": round(self.combined_absolute(), 4),
            "combined_for_reading": (None if reading is None
                                     else round(self.combined_for_reading(reading), 4)),
            "dominant_term": self.dominant_term(reading),
            "has_proportional_terms": self.has_proportional_terms,
            "assumption": self.assumption,
            "traceable": False,
        }


def budget_for(variable: str) -> Budget | None:
    """The budget for a variable, or None when there is none.

    None rather than a zero budget: "we have no numbers for this quantity" and "this
    quantity is exact" are different, and conflating them is how a report ends up
    quoting a precision nobody established.
    """
    raw = BUDGETS.get(variable)
    if raw is None:
        return None
    return Budget(
        variable=variable,
        unit=raw["unit"],
        components=dict(raw["components"]),
        assumption=raw["assumption"],
    )


def variables_with_budgets() -> list[str]:
    return sorted(BUDGETS)