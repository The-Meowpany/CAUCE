"""A subsystem with tests and no caller is not a subsystem.

`Esp32PeerExchange` and `SyncManager` were built, unit-tested and configured in `setup()`,
and `loop()` never drove them. STATUS row 12s records it. The whole LoRa/Wi-Fi sync path -
batching, signing, transport, acknowledgements - was verified by the host suite and had never
executed once on the product firmware.

Nothing in the build catches that. `loop()` compiles whether or not it calls `tick()`; the
host suite tests `SyncManager` directly and never sees `main.cpp`; and the gate builds the
image and checks its size, which is identical either way. A subsystem can be complete and
inert and every existing check reports green.

So the property is asserted here: each subsystem that exists in `main.cpp` must be driven from
`loop()`, not merely constructed in `setup()`. The check is textual because that is the only
place the fact lives - there is no runtime observer on a board that does not boot.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "firmware/src/main.cpp"

# Subsystem -> the expression whose presence in loop() means it is actually driven.
# Each of these was constructed in setup() and, at the time this file was written, never
# called from loop().
DRIVEN_FROM_LOOP = {
    "g_networkManager": "g_networkManager->tick()",
    "g_scheduler": "g_scheduler->tick()",
    "g_syncManager": "g_syncManager->tick()",
}


def _loop_body() -> str:
    source = MAIN.read_text(encoding="utf-8")
    start = source.index("void loop()")
    return source[start:]


def _setup_body() -> str:
    source = MAIN.read_text(encoding="utf-8")
    return source[source.index("void setup("):source.index("void loop()")]


def test_the_file_has_a_loop_to_assert_about():
    source = MAIN.read_text(encoding="utf-8")
    assert "void loop()" in source
    assert "void setup(" in source
    # If either stopped existing the checks below would pass vacuously, which is the failure
    # mode of every structural test written against source.
    assert len(_loop_body()) > 200, "loop() body looks too small to hold any driver"


def test_every_subsystem_built_in_setup_is_driven_from_loop():
    """The property, stated once so a new subsystem cannot repeat it.

    Construction is not use. A subsystem added to setup() and forgotten in loop() is the exact
    defect recorded in STATUS row 12s, and nothing else in this repository would notice.
    """
    setup = _setup_body()
    loop = _loop_body()
    built = set(re.findall(r"new cauce::[A-Za-z:]+", setup))
    assert built, "setup() constructs nothing, so this check would pass vacuously"

    unaddressed = []
    for expr in DRIVEN_FROM_LOOP:
        name = expr.split("->")[0]
        if name not in setup:
            continue  # not built here; nothing to assert
        if expr not in loop:
            unaddressed.append(name)
    assert not unaddressed, (
        f"built in setup() but never driven from loop(): {unaddressed}. "
        f"This is STATUS row 12s - a subsystem with tests and no caller.")


def test_the_sync_manager_is_actually_ticked():
    """Spelled out, because it is the one that was silently inert."""
    loop = _loop_body()
    assert "g_syncManager->tick()" in loop, (
        "the sync manager is configured in setup() and can be triggered by requestSyncNow(), "
        "but loop() never calls tick(). Everything from batching through signing to the "
        "transport exists and never runs.")


def test_the_tick_is_guarded_against_a_null_manager():
    """A tick that can dereference null is a crash, not a fix."""
    loop = _loop_body()
    at = loop.index("g_syncManager->tick()")
    guard = loop[max(0, at - 260):at]
    assert "g_syncManager != nullptr" in guard or "if (g_syncManager" in guard, (
        "loop() ticks the sync manager without checking it exists; on a node where setup "
        "bailed before constructing it, the first loop iteration would dereference null")


def test_the_tick_is_cheap_enough_to_call_every_loop():
    """Calling tick() on every pass is only acceptable if tick() decides for itself.

    If it returned early on an interval it does not check itself, the caller would need a
    timer and this would be a busy-wait. The guards are the contract.
    """
    source = (REPO / "firmware/lib/cauce_app/src/SyncManager.cpp").read_text(encoding="utf-8")
    body = source[source.index("void SyncManager::tick()"):]
    body = body[:body.index("\n}\n")]
    for guard in ("connected_", "halted_", "endpointConfigured_", "nextAttemptMonotonicMs_"):
        assert guard in body, (
            f"tick() no longer checks {guard}; if it stopped returning early when idle, "
            f"calling it from every loop iteration would do real work on every pass")


def test_the_weather_sensors_are_read_by_the_scheduler_not_by_hand():
    """The three sensors added this series must have a caller too.

    The scheduler owns the sampling cycle and calls `sensor->read()` for each variable a
    driver declares. Registering a driver in setup() is therefore enough - but only because
    the scheduler does that, and a driver wired anywhere else would be inert in exactly the
    way SyncManager was.
    """
    setup = _setup_body()
    for driver in ("Bh1750Driver", "Ccs811Driver", "HallAnemometerDriver"):
        assert f"new cauce::drivers::{driver}" in setup, f"{driver} is never constructed"
        assert "g_scheduler->addSensor(g_" in setup, "sensors are registered with the scheduler"

    scheduler = (REPO / "firmware/lib/cauce_app/src/MeasurementScheduler.cpp").read_text(
        encoding="utf-8")
    assert "sensor->read(variable, reading)" in scheduler, (
        "the scheduler has to be the thing that calls read(); without it a registered driver "
        "is constructed, begun, and never sampled")
