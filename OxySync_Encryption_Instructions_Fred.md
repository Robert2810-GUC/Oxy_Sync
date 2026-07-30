# OxySync Device — Encryption Setup Instructions

**For:** Fred Faiz  
**From:** Bharat  
**Date:** July 31, 2026  
**Device:** ESP32 DevKit V1 (NEW TEST UNIT ONLY)

---

## ⚠️ CRITICAL WARNING — READ BEFORE STARTING

- This process **permanently modifies** the device hardware (eFuse registers)
- It **CANNOT be undone** — ever
- Do this **ONLY on the new test unit** — NOT on any unit currently at the client site
- If the process is interrupted midway, the device may become permanently unusable
- Do NOT disconnect the USB cable during any step

---

## WHAT YOU RECEIVE FROM BHARAT

Create a folder on your Desktop called `OxySync_Encryption` and place all these files in it:

1. `oxysync_production.ino.bootloader.signed.bin`
2. `oxysync_production.ino.signed.bin`
3. `oxysync_production.ino.partitions.bin`
4. `boot_app0.bin`
5. `oxysync_public_key.pem`

---

## STEP 1 — Install Python (if not already installed)

1. Go to **https://www.python.org/downloads/**
2. Download and install the latest Python for Windows
3. **Important:** During installation, tick the box **"Add Python to PATH"**
4. Open Command Prompt and verify:
   ```
   python --version
   ```
   You should see a version number like `Python 3.x.x`

---

## STEP 2 — Install esptool

Open Command Prompt and run:

```
pip install esptool
```

Wait for it to finish. Then verify:

```
python -m esptool version
```

You should see `esptool.py v5.x.x` or similar.

---

## STEP 3 — Connect the device and find the COM port

1. Connect the ESP32 test unit to your PC via USB
2. Open **Device Manager** (right-click Start → Device Manager)
3. Look under **Ports (COM & LPT)**
4. Note the COM port number — e.g. `COM3` or `COM6`

You will replace `COMX` in all commands below with your actual COM port number.

---

## STEP 4 — Navigate to the files folder

Open Command Prompt and run:

```
cd C:\Users\%USERNAME%\Desktop\OxySync_Encryption
```

---

## STEP 5 — Burn the Secure Boot public key into eFuse

**This is permanent and irreversible.**

```
python -m espefuse --port COMX burn_key BLOCK2 oxysync_public_key.pem SECURE_BOOT_V2_RSA_3072
```

The tool will ask you to type **BURN** to confirm. Type it exactly and press Enter.

Expected output includes:
```
Burning efuses:
...
Check all blocks...
```

---

## STEP 6 — Enable Secure Boot V2

**This is permanent and irreversible.**

```
python -m espefuse --port COMX burn_efuse ABS_DONE_1
```

Again type **BURN** to confirm when prompted.

---

## STEP 7 — Flash the signed firmware

Run this single command (all one line):

```
python -m esptool --port COMX --baud 460800 write_flash 0x1000 oxysync_production.ino.bootloader.signed.bin 0x8000 oxysync_production.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 oxysync_production.ino.signed.bin
```

Wait for it to complete. You should see:
```
Hash of data verified.
Leaving...
Hard resetting via RTS pin...
```

---

## STEP 8 — Enable Flash Encryption

**This is permanent. The device will auto-encrypt its flash on next boot.**

```
python -m espefuse --port COMX burn_efuse FLASH_CRYPT_CNT
```

Type **BURN** to confirm.

---

## STEP 9 — Reboot and verify

1. Unplug and replug the USB cable (or press the EN/Reset button on the device)
2. The device will take slightly longer than usual on this first boot — this is normal (it is encrypting the flash)
3. On your phone or laptop, connect to WiFi network: **O2_Controller** (password: `techonly123`)
4. Open browser and go to: **192.168.4.1**
5. Login with: username `tech` / password `oxygen`
6. Confirm the dashboard loads and sensor readings appear

---

## STEP 10 — Report back to Bharat

Send Bharat a WhatsApp message confirming:
- [ ] Dashboard loaded successfully
- [ ] Sensor reading is showing
- [ ] Device WiFi is working
- [ ] Any errors or unexpected behaviour (if any)

---

## IMPORTANT — Future firmware updates

From this point on, **only signed firmware from Bharat** can be flashed to this device.  
Any unsigned firmware will be rejected and the device will not boot.  
Always contact Bharat for any future firmware updates.

---

*OxySync Encryption Setup — Bharat — July 31, 2026*
