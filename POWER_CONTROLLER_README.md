# Remote Power extension

This patch adds a separate HTTP dashboard (TCP/80) and a command socket (TCP/2323) while leaving the original USB serial bridge and Telnet listener (configured port, normally 23) untouched.

## GPIO assignment (ESP32-C3 SuperMini)

- GPIO4: relay module IN (active-LOW is assumed by this implementation).
- GPIO5: PC817 output; LOW means the optocoupler is conducting and PWR_OK is asserted.
- Relay VCC: regulated 5 V; relay GND: common ESP32 GND.
- Power the ESP32 independently from the target PC, e.g. a suitable USB supply. The target ATX +5VSB can be used for the sensing circuit only.

**Do not connect the relay to the motherboard until its polarity and idle state have been tested.** Confirm that GPIO4 starts with the relay de-energized and that the module's IN input is genuinely active-LOW. Some relay boards are active-HIGH.

## PWR_OK sensing

The previously discussed optocoupler/transistor circuit must be checked against the exact BC547 pinout and PC817 datasheet before wiring. ATX PWR_OK is a low-current logic output; do not drive the PC817 LED directly from PWR_OK with a 1 kOhm resistor. Use the BC547 as a switch powered from ATX +5VSB, with a series resistor for the PC817 LED and a base resistor/pulldown. Verify the actual PWR_OK polarity and current budget with a multimeter and datasheet. The firmware assumes PC817 output pulls GPIO5 LOW when PWR_OK is asserted.

## Commands

TCP 2323 uses one-line plaintext commands. Authenticate on each connection:

```text
PASS <sespass> STATUS
PASS <sespass> POWER ON
PASS <sespass> POWER OFF
PASS <sespass> POWER FORCE
```

Examples:

```bash
echo 'PASS YOUR_PASSWORD STATUS' | nc ESP32_IP 2323
echo 'PASS YOUR_PASSWORD POWER ON' | nc ESP32_IP 2323
echo 'PASS YOUR_PASSWORD POWER FORCE' | nc ESP32_IP 2323
```

HTTP dashboard: `http://ESP32_IP/`. Enter the existing `sespass` value. `POWER FORCE` asks for confirmation in the browser.

- `POWER ON`: 1 second pulse only if PWR_OK says OFF.
- `POWER OFF`: 1 second pulse only if PWR_OK says ON. This is a normal front-panel button press; OS behavior depends on OS power-button settings.
- `POWER FORCE`: 5 second press only if PWR_OK says ON. This can cause data loss.
- PWR_OK is an indication of ATX main rails, not proof that Linux/Windows is booted or responsive. ACPI sleep states may also leave PWR_OK asserted.

## Security and limitations

This is an initial LAN-only implementation, not suitable for exposure to the Internet. HTTP and TCP command authentication are not encrypted; use only on a trusted isolated LAN/VPN. Change the default `sespass` from `secret` during provisioning. The `/status` endpoint is unauthenticated and reveals only ON/OFF/busy. TCP 2323 accepts one command per connection.

## Build

Use the project's documented ESP-IDF v5.5.2 environment:

```bash
idf.py set-target esp32c3
idf.py build
```

The source archive was modified, but a firmware build has not been run in this environment because ESP-IDF (`idf.py`) is not installed here. Compile and inspect the build output before flashing hardware.
