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

Boot always starts with heating, fan, rotation and AUTO disabled. Only the
setpoint defaults to `26.7 C`. Existing installations keep target persistence
enabled. The desktop can disable it; in that mode a reboot restores the default
target while heater mode, AUTO and outputs always remain OFF.

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
| `05...` | provide external temperature in the `-40..80 C` range |
| `heater.status` | read-only detailed diagnostic snapshot |

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
