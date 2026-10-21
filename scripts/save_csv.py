#!/usr/bin/env python3
"""Export each group of 10 unexported EarthMeter tests from Firebase to CSV."""
import csv
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "data"
BATCH_DIR = DATA_DIR / "batches"
MASTER_CSV = DATA_DIR / "earthmeter_data.csv"
CURSOR_FILE = DATA_DIR / "last_exported_test.txt"
FIREBASE_BASE = os.environ.get(
    "FIREBASE_DATABASE_URL",
    "https://earth-meter-project-default-rtdb.asia-southeast1.firebasedatabase.app",
).rstrip("/")
BATCH_SIZE = 10
FIELDS = [
    "test_number",
    "current_mA",
    "ep_V",
    "resistance_firmware_value",
    "valid",
    "source",
    "exported_at_utc",
]


def fetch_records():
    url = f"{FIREBASE_BASE}/autoTests.json"
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=25) as response:
            raw = response.read().decode("utf-8")
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Firebase request failed: {exc}") from exc
    if raw == "null" or not raw.strip():
        return []
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError("Firebase returned invalid JSON") from exc
    if not isinstance(payload, dict):
        raise RuntimeError("Firebase /autoTests must be a JSON object keyed by test number")
    records = []
    for key, value in payload.items():
        if not isinstance(value, dict):
            continue
        try:
            number = int(value.get("testNumber", value.get("test", key)))
        except (TypeError, ValueError):
            continue
        records.append((number, value))
    return sorted(records, key=lambda item: item[0])


def read_cursor():
    try:
        return int(CURSOR_FILE.read_text(encoding="utf-8").strip() or "0")
    except FileNotFoundError:
        return 0
    except ValueError as exc:
        raise RuntimeError(f"Invalid cursor in {CURSOR_FILE}") from exc


def csv_row(number, record, exported_at):
    return {
        "test_number": number,
        "current_mA": record.get("current", ""),
        "ep_V": record.get("ep", ""),
        # Kept with an explicit name because the current firmware has a resistance
        # scaling inconsistency between its live display and persisted history.
        "resistance_firmware_value": record.get("resistance", ""),
        "valid": str(record.get("valid", "" )).lower(),
        "source": record.get("source", "ESP32-auto"),
        "exported_at_utc": exported_at,
    }


def write_csv(path, rows, append=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    needs_header = (not append) or (not path.exists()) or path.stat().st_size == 0
    mode = "a" if append else "w"
    with path.open(mode, newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        if needs_header:
            writer.writeheader()
        writer.writerows(rows)


def main():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    BATCH_DIR.mkdir(parents=True, exist_ok=True)
    cursor = read_cursor()
    pending = [(n, r) for n, r in fetch_records() if n > cursor]
    if len(pending) < BATCH_SIZE:
        print(f"Waiting for 10 new tests; found {len(pending)} (cursor={cursor}).")
        return 0

    # Export complete batches only. Any remainder stays pending for a later run.
    export_count = (len(pending) // BATCH_SIZE) * BATCH_SIZE
    ready = pending[:export_count]
    exported_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    batch_files = sorted(BATCH_DIR.glob("earthmeter_batch_*.csv"))
    next_batch = len(batch_files) + 1

    for offset in range(0, len(ready), BATCH_SIZE):
        batch = ready[offset : offset + BATCH_SIZE]
        rows = [csv_row(n, r, exported_at) for n, r in batch]
        batch_path = BATCH_DIR / f"earthmeter_batch_{next_batch:04d}.csv"
        write_csv(batch_path, rows)
        write_csv(MASTER_CSV, rows, append=True)
        next_batch += 1
        print(f"Wrote {batch_path.relative_to(ROOT)} ({len(rows)} tests).")

    new_cursor = ready[-1][0]
    CURSOR_FILE.write_text(f"{new_cursor}\n", encoding="utf-8")
    print(f"Exported {len(ready)} tests; cursor advanced from {cursor} to {new_cursor}.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
