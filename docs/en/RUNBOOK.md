# Runbook: what to do when something is wrong in the field

Written for whoever is on call at 03:00 and has never run this system before. Each
entry is a symptom you can recognise from a log line or a dashboard, the check that
confirms it, and the action.

The bias throughout is toward **not losing data**. A node that is silent is a
problem; a node that is deleted is a worse one. Nothing here destroys measurements.

---

## A node stopped reporting

**Recognise it:** the dashboard shows a site dark; `GET /v1/nodes` shows
`last_seen_utc_ms` hours old.

**Check, in this order.** Cheapest and most likely first.

1. Is the central up? `curl -k https://<host>/healthz`. If this fails, every node
   looks dead and none of them are.
2. Is the site reachable at all? Any node from that site reporting?
3. Is it one node or many? Many from one site points at the gateway or the radio.
   One points at the device.
4. Ask the device. Its local API answers on the LAN even with no uplink:
   `GET /api/v1/health` on the node's address gives `netState`, `rssiDbm`,
   `storedCount`, `measurementCount`. A node with a growing `storedCount` and a dead
   `netState` has data it cannot send — that is a radio problem, not a data problem,
   and the data is safe on the device.

**Act:** nothing destructive. A node that has been offline accumulates in flash and
syncs when it reconnects; `(node_id, sequence)` makes the catch-up idempotent, so a
node returning after a week does not need anything done to it. Note in the log that
it was silent, because a node offline past its storage budget starts dropping.

---

## Sync backlog is growing

**Recognise it:** `pending` on the node climbs, or the central's `sync_batches`
grows faster than it is acknowledged.

**Check:** is the node transmitting but not being acknowledged? `LoRaSyncTransport`
reports `lastDeliveryConfirmed()`. Frames going out and never confirmed means the
gateway is not hearing them, or the gateway is not forwarding — the central's own
`/v1/nodes/{id}` will show `last_seen` advancing even when measurements are not
landing.

**Act:** raise the spreading factor if the radio allows it. SF9 fits one record per
frame; each step up costs airtime and buys 3 dB. Do not raise the sync interval to
"catch up" — that makes the backlog worse.

---

## The central is down

**Act:** the nodes are unaffected. They measure, they validate, they store, they
serve their own dashboard. That is the design premise, not a consolation.

**Before starting it,** take a backup if the file system permits:

```
curl -k -H "Authorization: Bearer $TOKEN" \
  https://<host>/v1/maintenance/backup -o backup-$(date +%F).sqlite
```

**After starting it,** confirm migrations ran:

```
curl -k https://<host>/healthz
```

If a migration is what broke it, restore the previous database with
`backend/tools/restore.py` **with the service stopped**.

---

## Restore the central from a backup

```
# see what is in the file, change nothing
python backend/tools/restore.py --backup backup.sqlite --db ./data/cauce.sqlite --dry-run

# stop the service, then
python backend/tools/restore.py --backup backup.sqlite --db ./data/cauce.sqlite

# start the service, then
curl -k https://<host>/v1/nodes
```

The tool refuses a corrupt file, a file that is not a CAUCE database, and a backup
with fewer rows than the live one unless you pass `--yes`. That last refusal is the
one you will hit after restoring yesterday's file by accident; read the counts it
prints before overriding.

---

## A device is lost, stolen or compromised

**Act — retire it.** This is the one thing to do quickly.

```
curl -k -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"reason":"stolen"}' \
  https://<host>/v1/nodes/CAUCE-014/revoke
```

Its measurements stay. What is withdrawn is its right to contribute more.

**Confirm it worked:** a sync from that node now returns `403 node_retired`, on both
Wi-Fi and the relayed path.

**If the device is recovered and trustworthy**, it is a new device as far as the
central is concerned. Generate a new seed and provision again:

```
python backend/tools/provision.py --central https://<host> --count 1 \
  --prefix CAUCE --site rio-01 --out reseed.json
```

The old seed is dead. Do not reuse it.

---

## A node will not accept a command

**Recognise it:** the central keeps offering it; the node never acknowledges.

**Check:** `request_resync`, `set_sampling_interval`, `set_sync_interval` and
`set_led_mode` are the implemented kinds. Anything else is reported as an unknown
kind and will never be acknowledged — that is the central's bug, not the node's.

For the four real ones, the refusal reason is in the receipt detail:

| detail | meaning |
|---|---|
| `refused:sampling_out_of_range` | below 10 s or above 86400 s |
| `refused:sync_out_of_range` | below 60 s or above 86400 s |
| `refused:silence_budget_s=604800` | the pair would leave the node silent over a week |
| `refused:led_mode_out_of_range` | mode above 3 |
| `applied:...` | it worked; the value after `applied:` is what the node is now doing |

**A receipt reporting `applied:` is the value in force, not the value requested.**
If a command was clamped or refused, the node says so.

---

## An OTA update bricked a node

**Recognise it:** the node is unreachable and does not answer its local API.

**Check:** most "bricks" are a node that booted an image which never confirmed. The
rollback guard should return it to the previous partition on the next boot; if it
does not come back within two boot cycles, it needs physical access.

**Act:** reflash over USB. `docs/en/OTA.md` has the procedure.

**Prevention:** the anti-brick rules in `docs/en/OTA.md` exist so this cannot happen
from a bad download — hash, size and safety gates are checked before a byte is
written. It can still happen from an image that is valid and wrong, which is what the
boot-counter confirmation is for. **That mechanism is host-tested and unproven on
hardware**, so until the bench confirms it, treat every OTA as needing someone
nearby.

---

## Someone asks whether a node was compromised

The answer is bounded and the bounds are known:

- **HMAC nodes.** A node and the central share one secret. Anyone who has read the
  central's database can impersonate any HMAC node. Anyone who has read one node's
  flash can impersonate that node.
- **Ed25519 nodes.** The central stores only a public key, so reading the central's
  database does **not** let an attacker sign. Reading a node's flash **does**,
  because the seed is on the device.
- **Both.** Firmware signing does not exist. A node will accept and run any image
  that satisfies the manifest, so an attacker who can serve the manifest can run
  their own code on the node.

Those three sentences are the actual security posture. Anything more precise than
them is not implemented.

---

## Reporting a fault

Include, because every one of these has been needed to diagnose something:

- the node id and site
- `last_seen_utc_ms` from `GET /v1/nodes/{id}`
- `GET /api/v1/health` from the device if it answers
- the central's log line, verbatim
- whether the failure is a fixed node, a whole site, or everything

Fixed node points at the device. Whole site points at the gateway or the radio.
Everything points at the central or the link. That triage is three questions and it
resolves most of it.
