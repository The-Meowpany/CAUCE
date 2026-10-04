#!/usr/bin/env python3
"""Restore a CAUCE central from a backup, on the host, deliberately.

WHY THIS IS NOT AN HTTP ENDPOINT

A backup is taken through the API. Restoring is not. Restoring means overwriting the
live database, and an endpoint that can do that is an endpoint whose authentication
bug is a total loss of data. It belongs on the host, run by an operator who has
stopped the service and can see what they are doing.

WHAT IT CHECKS, AND WHY EACH CHECK IS THERE

- Integrity of the backup file. A restore from a corrupt file replaces a working
  database with a broken one, and the operator finds out days later.
- Table row counts before and after, printed. A restore that silently drops the
  measurements table is worse than a restore that refuses.
- Atomic replacement. The new file is prepared beside the old one and renamed over
  it, so a crash mid-restore leaves the original database intact rather than a
  half-written one.

    python tools/restore.py --backup cauce-backup.sqlite --db ./data/cauce.sqlite

Add --dry-run to see what would happen without touching anything.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

# Every table whose contents must survive a restore. Aggregates are rebuilt from
# raw rows on the next write, so they are counted but not required to match.
TABLES = (
    "nodes",
    "measurements",
    "sites",
    "agg_hourly",
    "agg_daily",
    "agg_15min",
    "sync_batches",
)


def integrity_ok(path: Path) -> str | None:
    """Returns None when the file is a sound SQLite database, else the reason."""
    if not path.exists():
        return "file does not exist"
    if path.stat().st_size == 0:
        return "file is empty"
    try:
        conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    except sqlite3.Error as exc:
        return f"cannot open: {exc}"
    try:
        row = conn.execute("PRAGMA integrity_check").fetchone()
        if row is None or row[0] != "ok":
            return f"integrity_check said {row[0] if row else 'nothing'}"
        present = {
            r[0]
            for r in conn.execute(
                "SELECT name FROM sqlite_master WHERE type='table'").fetchall()
        }
        missing = [t for t in TABLES if t not in present]
        if missing:
            return f"missing tables: {', '.join(missing)}"
        return None
    except sqlite3.DatabaseError as exc:
        return f"not a usable database: {exc}"
    finally:
        conn.close()


def row_counts(path: Path) -> dict[str, int]:
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        present = {
            r[0]
            for r in conn.execute(
                "SELECT name FROM sqlite_master WHERE type='table'").fetchall()
        }
        return {
            table: conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            for table in TABLES
            if table in present
        }
    finally:
        conn.close()


def restore(backup: Path, target: Path, assume_yes: bool = False) -> dict:
    """Replaces `target` with `backup`. Raises SystemExit rather than half-doing it."""
    problem = integrity_ok(backup)
    if problem is not None:
        raise SystemExit(f"refusing to restore: {problem}")

    before = row_counts(target) if target.exists() else {}
    after = row_counts(backup)

    lost = {t: before[t] for t in before
            if t in after and after[t] < before[t]}
    if lost and not assume_yes:
        raise SystemExit(
            "refusing to restore: the backup holds FEWER rows than the live "
            f"database for {lost}. Pass --yes if that is what you meant.")

    target.parent.mkdir(parents=True, exist_ok=True)
    handle, staged_name = tempfile.mkstemp(dir=str(target.parent),
                                          suffix=".restore-staged")
    os.close(handle)
    staged = Path(staged_name)
    try:
        shutil.copy2(str(backup), str(staged))
        # Copied, then verified again: the point is to catch a copy that did not
        # land, and the only way to know is to read the staged file.
        if integrity_ok(staged) is not None:
            raise SystemExit("staged copy did not verify; leaving the original alone")
        # os.replace is atomic within a filesystem, which is why the staged file is
        # created in the target's directory rather than in the system temp dir.
        try:
            os.replace(str(staged), str(target))
        except PermissionError as exc:
            # Windows refuses to replace a file that any process still holds open,
            # and SQLite holds the database open for as long as the server is
            # running. The raw error here is a PermissionError on a temp filename,
            # which tells the operator nothing about what to actually do.
            raise SystemExit(
                f"cannot replace {target}: it is open. Stop the CAUCE service "
                "first - restoring over a database the server is using is not "
                "supported, on any platform."
            ) from exc
    finally:
        if staged.exists():
            staged.unlink()

    final = row_counts(target)
    mismatched = {t: (after[t], final[t]) for t in after if final.get(t) != after[t]}
    if mismatched:
        raise SystemExit(f"restore landed but counts differ: {mismatched}")

    return {"target": str(target), "before": before, "after": final,
            "replaced": target.exists()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Restore a CAUCE central backup.")
    parser.add_argument("--backup", required=True, help="backup .sqlite file")
    parser.add_argument("--db", required=True, help="live database to replace")
    parser.add_argument("--yes", action="store_true",
                        help="proceed even if the backup has fewer rows")
    parser.add_argument("--dry-run", action="store_true",
                        help="verify and report; change nothing")
    args = parser.parse_args(argv)

    backup = Path(args.backup)
    target = Path(args.db)

    problem = integrity_ok(backup)
    if problem is not None:
        print(f"refusing to restore: {problem}", file=sys.stderr)
        return 1

    before = row_counts(target) if target.exists() else {}
    after = row_counts(backup)
    print(f"backup {backup} verifies")
    for table in TABLES:
        if table in after:
            was = before.get(table)
            print(f"  {table}: {after[table]}"
                  + (f" (live has {was})" if was is not None else " (live has none)"))

    if args.dry_run:
        print("dry run: nothing was written")
        return 0

    result = restore(backup, target, assume_yes=args.yes)
    print(f"restored into {result['target']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())