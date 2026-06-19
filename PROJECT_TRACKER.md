# OxySync Project Tracker

**Client:** Fred Faiz
**Team:** Bharat Sir (Megasoft)
**Project Start:** June 14, 2026
**Last Updated:** June 19, 2026

---

## What Is This Project?

**OxySync** is a standalone safety device that monitors ambient oxygen saturation in environments where oxygen is artificially elevated (e.g., oxygen therapy rooms). It is designed to comply with new legal safety requirements for oxygen cutoff systems — without replacing the existing oxygen control infrastructure.

**Hardware:**
- Microcontroller: ESP32 Devkit V1
- Sensor: Atlas Scientific ENO-02 (EZO O2, I2C address 0x6C)
- Relay: Active-low on GPIO 27
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
- [ ] **BUG 1:** Manual relay switching (ON/OFF → AUTO) incorrectly triggers the 30-minute lockout timer
- [ ] **BUG 2:** Reboot button not fully functional
- [ ] **BUG 3:** Logout button redirects to `/logout` and blocks re-login unless URL is manually edited

---

## Task List

### Priority 1 — Bug Fixes
- [ ] Fix lockout triggering on manual relay mode changes
- [ ] Fix reboot button
- [ ] Fix logout / re-login flow

### Priority 2 — Major New Features
- [ ] **Watchdog Timer** — hardware-level recovery if MCU or sensor locks up
- [ ] **Sensor Drift Tracking** — log readings over time, flag sensor for replacement
- [ ] **Live Sensor Status** — display on dashboard: OK / Disconnected / Stuck / Sleeping
- [ ] **Event Log** — timestamped log of relay changes, lockouts, calibrations, reboots (persist 30 days, survives reboot)

---

## Technical Notes

- `Sensor script_new.txt` is the reference for real I2C reads — it uses `sendToSensor("R", 900)` with 900ms delay vs the safety code's 1200ms delay. The new version uses `isPrintable()` for filtering.
- Event log needs persistent storage → likely NVS (Non-Volatile Storage) or SPIFFS/LittleFS on ESP32
- Watchdog: ESP32 has a built-in hardware watchdog — needs to be enabled and fed properly
- Sensor drift: Atlas Scientific EZO O2 datasheet should define accuracy/tolerance — monitor trend over time (e.g., ±0.5% drift over 7 days as threshold)
- Logout bug: HTTP Basic Auth on ESP32/WebServer doesn't have a clean logout mechanism — workaround needed (e.g., fake credentials challenge)

---

## Open Questions / Next Steps

1. Has any new code been written or shared after June 16?
2. Which task does Bharat want to tackle first — bug fixes or major features?
3. Sensor drift threshold — need to review the Atlas Scientific EZO O2 datasheet to define a reasonable default

---

## Communication Log

| Date | From | To | Summary |
|------|------|----|---------|
| Jun 14, 2026 | Fred | Bharat | Initial requirements + old code |
| Jun 14, 2026 | Bharat | Fred | Code review summary + 6 questions |
| Jun 16, 2026 | Fred | Bharat | Answers to all 6 questions + sensor script |

