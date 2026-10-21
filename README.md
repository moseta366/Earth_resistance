# EarthMeter automatic testing + CSV export

This project package contains the supplied EarthMeter ESP32 firmware and remote Firebase dashboard, updated for website-independent test scheduling and GitHub CSV export.

## What it does

- ESP32 starts a 60-second test every 5 minutes, with the first automatic test 5 minutes after boot.
- The schedule is on the ESP32; the website does not need to be open.
- Completed records are stored in the ESP32's existing Preferences history and uploaded to Firebase under `/autoTests/<testNumber>.json` when Wi-Fi is available.
- GitHub Actions checks Firebase every 5 minutes. When at least 10 new records are available, it creates a batch CSV in `data/batches/` and appends the rows to `data/earthmeter_data.csv`.
- The browser dashboard is for viewing/monitoring and is not required for the automatic measurement cycle.

## Repository layout

- `index.html` — supplied remote Firebase dashboard.
- `firmware/EarthMeter.ino` — ESP32 sketch with auto scheduling and per-test Firebase records.
- `scripts/save_csv.py` — reads Firebase `/autoTests.json` and exports groups of 10 records.
- `.github/workflows/earthmeter-csv.yml` — scheduled GitHub Actions workflow.
- `data/` — CSV output and export cursor are created here.

## Setup

1. Upload all files in this package to the root of `https://github.com/moseta366/Earth_resistance` on the `main` branch. Keep the `.github/workflows/` path exactly as shown.
2. In GitHub, open **Settings → Actions → General → Workflow permissions**. Allow workflows to have read and write permissions to the repository contents. The workflow declares `contents: write`, but repository/org policy can override it.
3. Open **Actions → EarthMeter CSV Export** and run **Run workflow** once to test the CSV exporter. It will say it is waiting until there are 10 unexported records if the batch is not ready.
4. Open `firmware/EarthMeter.ino` in Arduino IDE and select your ESP32 Dev Module / ESP32 WROOM board, then compile and upload to the ESP32. Keep the existing sensor wiring unchanged.
5. Power the ESP32 and connect it to Wi-Fi. The first auto test begins 5 minutes after boot. Tests continue while the ESP32 has power; no dashboard tab is required.
6. After ten new tests have reached Firebase, the next successful GitHub Actions run commits the CSV files to `data/`.

## Firebase requirements

The firmware currently uses the project's existing Firebase Realtime Database URL and unauthenticated test-mode requests. The exporter expects `/autoTests.json` to be readable by GitHub Actions. This package does not change Firebase rules. Public read/write rules are unsafe for production; use proper Firebase authentication/rules before deploying beyond a controlled test.

## Important limitations

- GitHub scheduled workflows are best-effort; a run can be delayed. They are not hard real-time.
- ESP32 must remain powered and connected to Wi-Fi to upload records. It can keep measuring with the website closed, but offline records are limited by the existing local ring buffer (latest 20 records).
- Current firmware has an inconsistency in resistance scaling between live display and persisted history. CSV column `resistance_firmware_value` intentionally preserves the exact stored value rather than claiming it is calibrated ohms. Verify/fix the scaling against a known reference before relying on CSV resistance values.
- This package has not been compiled on the user's hardware or tested against live sensors. Test with a safe, supervised setup before unattended use.
