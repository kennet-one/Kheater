# Kheater

ESP-IDF firmware for the KeeMASH heater node. The root `CMakeLists.txt` and
`main/` component are the canonical firmware. `IMG.h` retains the original
logo bitmap used by the SH1106 display.

## Hardware

- ESP32-D0WD-V3 rev 3.1
- 4 MB flash, no PSRAM
- fan: GPIO13, active high
- rotation: GPIO26, active high
- high heat relay: GPIO14, active low
- low heat relay: GPIO27, active low
- SH1106 I2C display: SDA21, SCL22, address `0x3c`
- AHT30 local temperature/humidity: same shared bus, address `0x38`, 3.3 V

All four outputs are initialized OFF before NVS, display, Wi-Fi or ESP-MESH
startup. The autonomous controller continues running if mesh startup fails.

## Runtime

- ESP-IDF 6.0.1, target `esp32`
- pinned `keemash_mesh_core v0.8.2`
- pinned U8g2 `2.37.1`
- A/B OTA layout with two 1984 KB app slots and rollback
- reliable V2 NODEINFO, TOPOLOGY, LOG, TASK, MEMORY, TIME, CONTROL and OTA
- legacy V1 text retained as a compatibility adapter

The SH1106 shows the original 64x64 power-on logo for three seconds, then a
live status view with mesh state, heater mode, current and target temperature,
output indicators and the most important safety or schedule state.

GPIOs always initialize OFF. Mode persistence (`M50/M51`) is a separate opt-in;
saved AUTO waits for a new valid temperature and manual heat retains its cutoff.
Target persistence (`P50/P51`) is independent; default target is `26.7 C`.

Local AHT30 is the default AUTO source. An explicitly configured zone may use
external temperature while fresh; loss/invalidity falls back to AHT30. Two fresh
external samples are required to switch back. If neither source is usable,
AUTO stops heat/rotation and runs the existing cooldown. Internal/external
freshness defaults are 10/30 seconds. Humidity never controls heating.
The display shows INT/ZONE and local RH; shared I2C acquisition continues even
when the display is off. Typed SENSOR and HC1 events identify local data.

## Commands

| Command | Action |
| --- | --- |
| `he0` | fan |
| `he1` | fan + low heat |
| `he2` | fan + high heat |
| `he3` | fan + low + high heat |
| `he4` | heat and rotation OFF, fan cooldown, then full OFF |
| `he5` | enable AUTO and wait for a new valid temperature |
| `hero` | toggle rotation when no cooldown is active |
| `heho` | publish legacy mode, rotation and setpoint state |
| `W5...` | set target in the `5..35 C` range |
| `P50`, `P51` | disable or enable target persistence across reboot |
| `05...` | rejected: use the configured zone source, never unqualified input |
| `heater.status` | read-only detailed diagnostic snapshot |
| `heater.climate?` | read-only local/zone source, temperature, humidity and age |

`heater.status` returns a bounded `H6` result that fits the reliable CONTROL
cache. Fields are `m` mode, `a` AUTO, `sp` setpoint x10, `tp` target
persistence, `mp` mode persistence, `tv/t/ta` temperature validity/value x10/age
seconds, `o` output bit mask (fan/low/high/rotation), `cd` cooldown seconds,
`man` manual-run seconds, `sr` stop reason and `tc` timeout count.

Configure persistent source bindings on node0 through paired KeeLink using
`heater.source?`, `heater.source:internal`, or
`heater.source:zone:<source12hex>`, targeted to the heater MAC. Internal HC/HT/HX
messages are accepted only through authenticated-root reliable CONTROL.
Run `tools\test-climate.cmd` for portable deterministic policy tests (MSVC).

Manual heat has a four-hour one-shot limit. Invalid or stale AUTO temperature
turns both heat outputs and rotation OFF and runs the fan for 30 seconds.

## Build And Flash

Use the shared MASH helper so ESP-IDF and PowerShell execution policy are
configured consistently:

```powershell
tools\idf.cmd -ProjectPath "C:\Users\kennet\Desktop\To Git\Kheater" -Jobs 16 build
tools\idf.cmd -ProjectPath "C:\Users\kennet\Desktop\To Git\Kheater" -Port COM13 flash
```

Keep router credentials in the ignored local `sdkconfig`. Never commit them.
Perform the first flash and all GPIO/relay checks with the 230 V load
disconnected.
