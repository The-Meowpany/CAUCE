"""Identifier shape, in one place, because there are three doors into it.

`node_id` reaches the `nodes` table from `/v1/sync`, `/v1/provision` and the LoRa relay.
Each had its own idea of what to check, and `node_id` had the weakest of them: a non-empty
string. `variable` and `sensor_id` were validated against a character class because they are
rendered by the dashboard. `node_id` is rendered too, exported, and used as a filename, and it
had no check at all.

Confirmed against the code before changing it: a node could sync as `=cmd|'/C calc'!A0` and
the central answered `200`. That identifier then reached `/v1/export-all.csv` unquoted in the
first column, so an operator opening the export in a spreadsheet evaluated a formula instead
of reading a node id. Stored, and it fires on whoever opens the file next - nobody who can see
the cause.

One module rather than three inline checks. A shape check applied at two of three doors is a
check waiting to be forgotten at the third, and the third door here is a radio.
"""

from __future__ import annotations

import re

from fastapi import HTTPException

# Letters, digits, underscore, dot and dash, bounded to 64 characters.
#
# The class is shared with `variable` and `sensor_id` on purpose. One identifier syntax in a
# payload is one thing to audit.
#
# What it excludes and why each one matters here:
#
#   `=` `+` `-` `@`   a spreadsheet treats a leading one of these as a formula, and the CSV
#                     export writes this value into the first column unquoted.
#   `"` `'`           a quote breaks CSV quoting; the firmware's escaper doubles `"` but a
#                     value that never arrives is one less thing to escape.
#   `,` `\n` `\r`     structural CSV characters.
#   `<` `>` `\`       markup. The dashboard escapes on output, so this is belt and braces for
#                     the XSS that `variable` was already validated against.
#
# The length bound is 64 because this identifier is used in a `Content-Disposition` filename
# and in bounded device-side buffers. Unbounded means a node can sync an id that is silently
# truncated somewhere, which is a data-integrity bug rather than a security one.
NAME_PATTERN = re.compile(r"[A-Za-z0-9_.-]{1,64}")

# Kept as a separate name because it reads better at the validation sites and because
# `NAME_PATTERN` is imported by tests that want to assert the class itself.
NODE_ID_PATTERN = NAME_PATTERN


def require_node_id(node_id) -> str:
    """Validates a node identifier and returns it unchanged.

    Raises 422. `missing_node_id` when absent or not a string, `invalid_node_id` when the
    shape is refused - distinct codes because an operator who pasted `=cmd|calc` needs to be
    told the shape was refused, not that nothing arrived.
    """
    if not isinstance(node_id, str) or not node_id:
        raise HTTPException(status_code=422, detail="missing_node_id")
    if not NAME_PATTERN.fullmatch(node_id):
        raise HTTPException(status_code=422, detail="invalid_node_id")
    return node_id


def require_name(value, field: str, *, optional: bool = False) -> str | None:
    """Validates a `variable` or `sensor_id`, which share the character class.

    These were validated inline in the sync handler. Extracted so the class and the check
    live beside `require_node_id` and are asserted together rather than in one place and
    trusted in another.
    """
    if value is None and optional:
        return None
    if not isinstance(value, str) or not NAME_PATTERN.fullmatch(value):
        raise HTTPException(status_code=422, detail=f"invalid_{field}")
    return value
