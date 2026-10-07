# Calibration procedure

How to get a node's readings onto a defensible scale, and how to know when not to.

The tool is `backend/tools/calibrate.py`. This document is the part that cannot be automated:
what a good co-location looks like, which results to accept, and which to throw away.

## Why calibration exists at all

A BME280 is not a thermometer. Its absolute accuracy is roughly ±1 °C out of the box, which is
the same order as the between-day variation the system exists to observe. A field with a 0.3 °C
diurnal swing cannot have that swing measured by an uncalibrated sensor, and cannot have it
measured by a badly calibrated one either — a linear correction does not remove hysteresis,
and a BME280 warms up and drifts.

So calibration is not a nicety here; without it the central cannot distinguish a real change in
the environment from a change in the sensor.

## The procedure

### 1. Co-locate

One node and one reference instrument, **in the same air**, for at least 48 hours.

"Same air" is the whole difficulty and it is physical:

- **Radiation shield.** A node in direct sun reads several degrees above the air temperature.
  This is the most common cause of a bad calibration and it does not look bad on the plot — the
  correlation stays high while the offset tracks the weather. Put both under a ventilated
  white shield, or at minimum out of direct sun and off a warm wall.
- **Height.** Within about 30 cm, or you are measuring a gradient.
- **Ground.** Not on the ground. A node on bare ground sees the ground's temperature, and the
  ground has a much larger diurnal swing than the air.
- **Enclosure.** If the node is in a sealed box, the box is the instrument. Calibrate what is
  actually deployed.

### 2. Collect

```sh
python tools/calibrate.py --site SITE-01 --variable air_temperature \
    --reference NODE-REF --hours 72 --json > calibration-raw.json
```

72 hours rather than 48 if you can spare it. Two full diurnal cycles is the minimum; a single
cycle cannot separate a linear correction from a phase shift, and the tool will fit a linear
correction that is confidently wrong.

The reference must itself be calibrated. A calibration against an uncalibrated reference is a
second-order effect that this procedure does not detect.

### 3. Read the report before accepting it

The report carries a pass fraction, an uncertainty, and the fit itself. **The pass fraction is
the gate.** `calibrate.py` reports it as a number, and the procedure is: below 0.90, throw the
run away and find out why.

| pass fraction | reading | action |
|---|---|---|
| ≥ 0.98 | excellent | accept |
| 0.90 – 0.98 | acceptable | accept, note it |
| 0.70 – 0.90 | poor | reject; the co-location was not co-located |
| < 0.70 | failed | reject and investigate the hardware |

A low pass fraction is not noise. It is the report saying that the two instruments did not see
the same thing, and the usual causes are in this order:

1. One of them was in direct sun for part of the window.
2. They were not at the same height, or one was on the ground.
3. One lost Wi-Fi for part of the window, so its timestamps are wrong and it is being compared
   against the wrong hour. Check the sample counts; a run that silently drops half a day is the
   version of this that costs a week.
4. The reference itself drifted.

### 4. Fit

Default is offset-only, which is `measured = raw + offset`.

`--fit-scale` adds a slope, by least squares. Use it when the residuals show curvature — the
error is proportional to reading rather than constant — which is common for BME280 humidity.

**A slope near 1.0 with a large offset is the normal result.** A slope far from 1.0 means one
instrument is not linear over this range, which for a BME280 usually means it is out of its
specified operating band. Check the range before accepting a slope.

### 5. Apply

```sh
python tools/calibrate.py --site SITE-01 --variable air_temperature \
    --reference NODE-REF --hours 72 --apply --central https://central.example --token "$ADMIN_TOKEN"
```

`--apply` refuses without an admin token, and it is refused by the API without one as well.

### 6. Verify, which is not optional

Apply to **one node**, wait for a full diurnal cycle, then compare that node against the
reference again *without* applying anything.

- The residual drops: accept, then apply to the rest of the site.
- The residual does not drop: the fit was wrong, or the correction was applied to a different
  variable, or the node drifted after calibration. Undo it and start again.

Applying to the whole site and finding out afterwards is how a bad offset becomes a week of
data that reads as a climate signal.

## Re-calibration

Every **90 days**, and immediately after:

- a firmware update that changes the sensor driver,
- a sensor replacement,
- a move of the node to a different position,
- a change to the enclosure or the shield.

Sensor replacement invalidates the calibration by definition. A node that has been moved is
measuring something else and the old offset is worse than no offset, because it is confidently
wrong.

## What calibration cannot fix

Stated here because the temptation is to keep iterating on the fit:

- **Hysteresis.** The BME280 reads differently on the way up than on the way down. A linear
  correction is symmetric by construction and cannot represent this.
- **Condensation.** A node that gets wet reads wrong in a way no offset corrects.
- **Self-heating.** A node in a sealed enclosure with a Wi-Fi radio warms its own air. This is
  a fixed bias an offset *can* remove, but the fix is the enclosure, and removing the bias
  hides a problem that will get worse.
- **A failed sensor.** A stuck BME280 produces a flat line, and a flat line can produce an
  excellent-looking fit against another flat line. Check that the reference actually moved
  during the window before accepting anything.

## Provenance

Every applied calibration records its reference node, its window, its fit parameters and the
uncertainty. That is what makes a later question — "was this value corrected, and when?" —
answerable, and it is why the central refuses to accept a calibration without a reference.