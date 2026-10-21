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

# Project paths
ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "data"
BATCH_DIR = DATA_DIR / "batches"

MASTER_CSV = DATA_DIR / "earthmeter_data.csv"
CURSOR_FILE = DATA_DIR / "last_exported_test.txt"

# Firebase configuration
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
    """Fetch test records from Firebase."""

    url = f"{FIREBASE_BASE}/autoTests.json"

    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json"},
    )

    try:
        with urllib.request.urlopen(request, timeout=25) as response:
            raw = response.read().decode("utf-8")

    except urllib.error.URLError as exc:
        raise RuntimeError(
            f"Firebase request failed: {exc}"
        ) from exc

    if raw.strip() == "null" or not raw.strip():
        print("Firebase /autoTests is empty.")
        return []

    try:
        payload = json.loads(raw)

    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "Firebase returned invalid JSON."
        ) from exc

    # Firebase may return either an object or an array.
    if isinstance(payload, dict):
        items = payload.items()

    elif isinstance(payload, list):
        items = enumerate(payload, start=1)

    else:
        raise RuntimeError(
            "Unexpected Firebase /autoTests format: "
            f"{type(payload).__name__}. Expected object or array."
        )

    records = []

    for key, value in items:

        if not isinstance(value, dict):
            continue

        try:
            number = int(
                value.get(
                    "testNumber",
                    value.get("test", key),
                )
            )

        except (TypeError, ValueError):
            print(f"Skipping record with invalid test number: {key}")
            continue

        records.append((number, value))

    records.sort(key=lambda item: item[0])

    print(f"Firebase records found: {len(records)}")

    return records


def read_cursor():
    """Read the last exported test number."""

    try:
        content = CURSOR_FILE.read_text(
            encoding="utf-8"
        ).strip()

        return int(content or "0")

    except FileNotFoundError:
        return 0

    except ValueError as exc:
        raise RuntimeError(
            f"Invalid test cursor in {CURSOR_FILE}"
        ) from exc


def csv_row(number, record, exported_at):
    """Convert a Firebase record into a CSV row."""

    return {
        "test_number": number,
        "current_mA": record.get("current", ""),
        "ep_V": record.get("ep", ""),
        "resistance_firmware_value": record.get(
            "resistance", ""
        ),
        "valid": str(
            record.get("valid", "")
        ).lower(),
        "source": record.get(
            "source", "ESP32-auto"
        ),
        "exported_at_utc": exported_at,
    }


def write_csv(path, rows, append=False):
    """Write CSV rows, creating parent folders when needed."""

    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    needs_header = (
        not append
        or not path.exists()
        or path.stat().st_size == 0
    )

    mode = "a" if append else "w"

    with path.open(
        mode,
        newline="",
        encoding="utf-8",
    ) as handle:

        writer = csv.DictWriter(
            handle,
            fieldnames=FIELDS,
        )

        if needs_header:
            writer.writeheader()

        writer.writerows(rows)


def main():
    """Export complete batches of 10 new tests."""

    DATA_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    BATCH_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    cursor = read_cursor()

    print(f"Last exported test number: {cursor}")

    all_records = fetch_records()

    pending = [
        (number, record)
        for number, record in all_records
        if number > cursor
    ]

    print(f"New tests waiting for export: {len(pending)}")

    if len(pending) < BATCH_SIZE:
        print(
            f"Waiting for {BATCH_SIZE} new tests. "
            f"Currently available: {len(pending)}."
        )

        return 0

    # Export only complete batches.
    export_count = (
        len(pending) // BATCH_SIZE
    ) * BATCH_SIZE

    ready = pending[:export_count]

    exported_at = datetime.now(
        timezone.utc
    ).isoformat(timespec="seconds")

    batch_files = list(
        BATCH_DIR.glob("earthmeter_batch_*.csv")
    )

    next_batch = len(batch_files) + 1

    # Write batch CSVs and append rows to the master CSV.
    for offset in range(
        0,
        len(ready),
        BATCH_SIZE,
    ):

        batch = ready[
            offset:offset + BATCH_SIZE
        ]

        rows = [
            csv_row(number, record, exported_at)
            for number, record in batch
        ]

        batch_path = BATCH_DIR / (
            f"earthmeter_batch_{next_batch:04d}.csv"
        )

        write_csv(
            batch_path,
            rows,
        )

        write_csv(
            MASTER_CSV,
            rows,
            append=True,
        )

        print(
            f"Created {batch_path.relative_to(ROOT)} "
            f"with {len(rows)} tests."
        )

        next_batch += 1

    # Update cursor after successful CSV writes.
    new_cursor = ready[-1][0]

    CURSOR_FILE.write_text(
        f"{new_cursor}\n",
        encoding="utf-8",
    )

    print(
        f"Export completed. "
        f"Exported {len(ready)} tests; "
        f"cursor advanced from {cursor} to {new_cursor}."
    )

    return 0


if __name__ == "__main__":

    try:
        sys.exit(main())

    except Exception as error:
        print(
            f"ERROR: {error}",
            file=sys.stderr,
        )

        sys.exit(1)
