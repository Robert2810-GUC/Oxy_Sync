# OxySync Project Tracker

**Client:** Fred Faiz
**Team:** Bharat Sir (Megasoft)
**Project Start:** June 14, 2026
**Last Updated:** July 24, 2026 (Encryption task approved by Fred)

---

## What Is This Project?

**OxySync** is a standalone safety device that monitors ambient oxygen saturation in environments where oxygen is artificially elevated (e.g., oxygen therapy rooms). It is designed to comply with new legal safety requirements for oxygen cutoff systems — without replacing the existing oxygen control infrastructure.

**Hardware:**
- Microcontroller: ESP32 Devkit V1
- Sensor: Atlas Scientific ENO-02 (EZO O2, I2C address 0x6C)
- Relay: Active-high on GPIO 27 (DFRobot MOSFET DFR0457)
- Access: WiFi AP (`O2_Controller` / `techonly123`), browser at `192.168.4.1`
- Dashboard login: `tech` / `oxygen`

**How It Works:**
1. Monitors O2 level continuously (every 10 seconds)
2. If O2 drops below setpoint (default 20.9%) → relay activates
3. Once O2 reaches setpoint → relay deactivates → 30-minute lockout begins
4. Lockout prevents relay from re-triggering while O2 stabilizes
5. Technician can connect via WiFi and view/control via web dashboard

---

## Source Files

| File | Description |
|------|-------------|
| `Sensor safety_old.txt` | Main OxySync safety controller code (current version to be improved) |
| `Sensor script_new.txt` | Client's original sensor-only code from existing oxygen control system — reference for real I2C sensor reads |

---

## Chat History Summary

### June 14, 2026 — Initial Brief (Fred → Bharat)
Fred sent the project requirements email via WhatsApp. Key points:
- This is a new safety device product
- Already has base code working (WiFi AP, dashboard, relay logic, BT serial)
- Listed bugs and new feature requests

### June 14, 2026 — Code Review & Clarifications (Bharat → Fred)
Bharat reviewed the existing code and sent back:
- Summary of what is working
- 6 clarification questions
- Note about hardcoded O2 value (19.5%) being a test placeholder

### June 16, 2026 — Client Answers (Fred → Bharat)
Fred answered all 6 clarification questions:

| # | Question | Answer |
|---|----------|--------|
| 1 | Real sensor code? | Attached — the `Sensor script_new.txt` file |
| 2 | WiFi AP confirmed? | Yes, confirmed working. IP: 192.168.4.1 |
| 3 | Setpoint adjustable? | Yes — field-adjustable preferred to avoid factory returns |
| 4 | Unique device naming? | Bharat will set name per unit at upload time |
| 5 | Event log survive reboot? | Yes — keep 30 days max, auto-delete older entries |
| 6 | Sensor drift threshold? | Fred unsure — defer to Bharat's judgment (datasheet linked) |

---

## Current Code Status (`Sensor safety_old.txt`)

### Working / Implemented
- [x] Core relay logic (activate/deactivate based on O2 setpoint)
- [x] 30-minute lockout timer after relay deactivation
- [x] Web dashboard with Basic Auth login (tech/oxygen)
- [x] Manual relay override (ON/OFF/AUTO) with 5-minute auto-timeout
- [x] WiFi Access Point mode (O2_Controller / techonly123)
- [x] Bluetooth Serial interface for tech commands
- [x] I2C sensor communication with Atlas Scientific ENO-02
- [x] Calibration command via dashboard (`CAL,20.9`)
- [x] Reboot endpoint (`/reboot`)
- [x] Logout endpoint (`/logout`)

### Known Bugs
- [x] **BUG 1:** Manual relay switching (ON/OFF → AUTO) incorrectly triggers the 30-minute lockout timer — FIXED: `relayAutoActive` flag ensures lockout only fires when AUTO logic activated the relay; any manual ON/OFF action clears the flag first
- [x] **BUG 2:** Reboot button not fully functional — FIXED: restart deferred to `loop()` via `pendingReboot` flag (500ms after response sent); JS now shows "Rebooting…" screen and polls `/status` every 3s until device is back, then reloads
- [x] **BUG 3:** Logout button redirects to `/logout` and blocks re-login — FIXED: JS now navigates to `/logout` (not fetch); login and logout both use realm `"OxySync"` so the browser clears the correct credential cache; logout page body has the re-login link

---

## Task List

### Priority 1 — Bug Fixes
- [x] Fix lockout triggering on manual relay mode changes
- [x] Fix reboot button
- [x] Fix logout / re-login flow

### Priority 2 — Major New Features (ALL DONE)
- [x] **Watchdog Timer** — DONE: `esp_task_wdt_init(30s, panic=true)` in setup, `esp_task_wdt_reset()` fed every loop. Reboots on any 30s stall.
- [x] **Sensor Drift Tracking** — DONE: 30-day circular buffer of daily averages in NVS (`Preferences`). ±0.3% CAUTION, ±0.6% REPLACE. Wokwi has `/testday` + "Force Day" button to test without waiting 24h. Calibrate resets history.
- [x] **Live Sensor Status** — DONE: codes 4=Disconnected, 5=Stuck, 6=Sleeping in dashboard. Disconnection detected via I2C endTransmission() error. Stuck = same reading 6+ reads (~1 min).
- [x] **Event Log** — DONE: LittleFS /log.txt, max 300 entries (trimmed to 200). Browser auto-syncs Unix time on every load. /logpage shows colour-coded table. /clearlog to wipe. Survives reboot via 10-min NVS timebase save.

### Priority 3 — Encryption & Re-deployment (APPROVED Jul 24, 2026)
- [ ] **ESP32 Flash Encryption + Secure Boot** — burn eFuses, generate RSA signing key, produce encrypted+signed firmware binary
- [ ] **Re-flash all units** with encrypted firmware
- [ ] **Key storage procedure** — document where signing key lives; key loss = no future updates possible
- [ ] **Verify update flow** — confirm new encrypted builds can be flashed over existing encrypted firmware (same key)

> **CRITICAL:** Flash encryption on ESP32 is permanent (eFuse-based). Once burned, it cannot be undone. Every future firmware update must be signed with the same key generated during initial setup. Fred has been informed and approved.

---

## Testing Workflow (IMPORTANT)

1. **Load `oxysync_wokwi.ino` into Wokwi** with `wokwi/diagram.json`
2. Open the dashboard at `192.168.4.1`, log in with `tech` / `oxygen`
3. Use the **Simulator card** (slider) to set O2 below 20.9% → confirm relay activates
4. Raise O2 above 20.9% → confirm relay deactivates and lockout starts
5. Test manual ON/OFF → AUTO without lockout triggering
6. Test logout → confirm login prompt appears on return to `/`
7. Test reboot → confirm page shows "Rebooting…" and auto-reloads when device is back
8. **Only after all above pass** → flash `oxysync_production.ino` to real hardware
9. Test once on real hardware, then send to Fred for client testing

---

## Technical Notes

- `Sensor script_new.txt` is the reference for real I2C reads — it uses `sendToSensor("R", 900)` with 900ms delay vs the safety code's 1200ms delay. The new version uses `isPrintable()` for filtering.
- Event log: DONE — LittleFS `/log.txt`, max 300 entries, trimmed to 200 on overflow, survives reboot via NVS timebase
- Watchdog: DONE — `esp_task_wdt` 30s timeout, panic=true, `esp_task_wdt_reset()` every loop()
- Sensor drift: DONE — ±0.3% CAUTION / ±0.6% REPLACE thresholds, 30-day NVS circular buffer
- Logout bug: FIXED — JS navigates to `/logout`, same realm `"OxySync"` clears browser credential cache, re-login link on logout page
- Encryption (NEXT): ESP32 Flash Encryption (AES-256) + Secure Boot V2 (RSA-3072). eFuse-based, irreversible. Signing key must be kept permanently.

---

## Current Status

**ALL P2 FEATURES COMPLETE. P3 Encryption task APPROVED by Fred (Jul 24, 2026).**
- Fred asked: "Can you encrypt and re-deploy the shutdown software?"
- Bharat sent timeline/cost doc: `OxySync_Encryption_Timeline.docx`
- Fred asked: "Would we be able to flash over the encrypted file with new encrypted updated file?"
- Bharat answered: Yes — updates are always possible as long as the same signing key is kept safe
- Fred approved — encryption work is now the active task

## Open Questions / Next Steps

1. Generate RSA signing key and store it securely before touching any hardware
2. Build encrypted + signed firmware binary from `oxysync_production.ino`
3. Test full update cycle (flash encrypted → flash new encrypted version) before touching client units
4. Document key storage procedure for Bharat's records

---

## Communication Log

| Date | From | To | Summary |
|------|------|----|---------|
| Jun 14, 2026 | Fred | Bharat | Initial requirements + old code |
| Jun 14, 2026 | Bharat | Fred | Code review summary + 6 questions |
| Jun 16, 2026 | Fred | Bharat | Answers to all 6 questions + sensor script |
| Jun 19, 2026 | Claude | Bharat | All 3 P1 bugs fixed in both production and Wokwi firmware |
| Jun 19, 2026 | Claude | Bharat | UI redesigned v2 — two-column no-scroll layout, navy/white formal palette, full-width, color only on status values |
| Jun 19, 2026 | Bharat | Fred   | Sent production firmware for client testing — bug fixes + UI redesign. P2 features on hold pending feedback. |
| Jun 19, 2026 | Claude | Bharat | Sensor Drift Tracking complete in both production and Wokwi. Wokwi adds /testday + "Force Day" button for testing. |
| Jul 24, 2026 | Fred   | Bharat | Asked to encrypt and re-deploy the OxySync firmware. Asked if future updates would still be possible after encryption. |
| Jul 24, 2026 | Bharat | Fred   | Sent OxySync_Encryption_Timeline.docx with timeline and cost. Confirmed updates are possible with same signing key. Warned encryption is permanent and key must be kept safe. |
| Jul 24, 2026 | Fred   | Bharat | Approved encryption task. |
| Jul 24, 2026 | Bharat | Fred   | Sent 4 pre-encryption questions via WhatsApp: (1) number of units, (2) OS on flashing machine, (3) Arduino IDE version or other tool, (4) USB direct flash confirmation. Awaiting reply before starting. |
| Jul 25, 2026 | Fred   | Bharat | Answered all 4 questions. (1) 0 units now — will build new test unit; (2) Windows 11 64-bit; (3) Arduino IDE 2.3.6; (4) Yes USB direct, has both Micro USB and USB-C DevKit V1. Invoice to go to Ron. Mentioned possible future switch to ESP32-C3. |

