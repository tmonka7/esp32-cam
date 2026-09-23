# ESP32-S3 CAM Gateway

Firmware for a **Freenove / Goouuu ESP32-S3-CAM** (ESP32-S3-WROOM-1 N8R8 + OV2640)
that combines four things on one board:

| Feature | Where it lives |
|---|---|
| Camera, MJPEG stream and stills over HTTP | `http://<ip>/` and `http://<ip>:81/stream` |
| Modbus **RTU master** over RS-485 / TTL UART | UART1, polled table + one-shot transactions |
| Transparent UART bridge | UART2, read/write over HTTP or BLE |
| Runtime GPIO control | any free pin, persisted in NVS |
| BLE command console | Nordic UART Service, works with no Wi-Fi |

Built for **ESP-IDF v5.3.x** (anything ≥ 5.1 should work).

---

## Build and flash

**This project builds offline.** Nothing is fetched from the ESP component
registry — both third-party components are committed under `components/`.

```bash
idf.py set-target esp32s3
idf.py menuconfig          # ESP32-S3 CAM Gateway → Wi-Fi → SSID/password
idf.py build flash monitor
```

A working ESP-IDF v5.3.x installation is the only prerequisite.

### Vendored components

| Path | Version | Why pinned there |
|---|---|---|
| `components/esp32-camera` | v2.0.15 | Last release with **no** external dependencies. v2.1.x pulls in `espressif/esp_jpeg`, which would mean vendoring a third component. |
| `components/esp-modbus` | v1.0.9 | Newest 1.0.x tag. The 2.x line replaced the `mbc_master_*` controller API used by `app_modbus.c`. |

Each carries a `VENDORED.txt` recording its upstream URL, tag and commit.
Development scaffolding (`.github`, `test`, `docs`, `examples`) was stripped;
everything the component's own `CMakeLists.txt` references is present.

To refresh them — the **only** step that needs network access:

```powershell
pwsh tools/vendor_components.ps1
```

Then commit `components/`.

### How the offline guarantee is enforced

- **No project manifest.** There is no `main/idf_component.yml`. The component
  manager has nothing to resolve, so it never reaches the registry. The
  ESP-IDF version requirement that used to live there is now a check in the
  root `CMakeLists.txt`.
- **The two manifests that remain** are upstream files inside the vendored
  trees, and both declare only `idf`, which always resolves locally.
- **`CMakeLists.txt` fails fast** with a readable message if `components/` is
  missing, instead of a wall of "esp_camera.h: No such file or directory".
- **`.gitignore` patterns are anchored** to the project root. An unanchored
  `build/` or `sdkconfig` would also match inside `components/` and silently
  drop vendored files from the repo.

Do not re-add registry dependencies for these two: a registry entry alongside
a local component of the same name is a conflict, and it puts the build back
on the network.

Verify at any time — both should print nothing:

```bash
# Registry dependencies are indented "namespace/name:" entries. Matching on
# "espressif/" alone would false-positive on the url/repository metadata that
# every upstream manifest carries.
grep -rnE "^[[:space:]]+[a-z0-9_]+/[a-z0-9_-]+:" --include=idf_component.yml .

# Vendored files that .gitignore would drop from the repo.
git ls-files --others --ignored --exclude-standard components/
```

If the board cannot join the configured network it brings up a fallback SoftAP
(`esp32s3-cam` / `12345678` by default) and keeps retrying the station in the
background, so a router reboot does not need a board reboot.

### If your module is not an N8R8

`sdkconfig.defaults` assumes 8 MB flash and **octal** PSRAM. For an N8R2 (quad
PSRAM) change `CONFIG_SPIRAM_MODE_OCT=y` to `CONFIG_SPIRAM_MODE_QUAD=y`, delete
`sdkconfig`, and rebuild. Without working PSRAM the camera falls back to QVGA
with a single frame buffer — it still runs, just small.

---

## Wiring

### Camera (fixed by the board, listed for reference)

```
XCLK  15    SIOD  4     SIOC  5
D7    16    D6    17    D5    18    D4    12
D3    10    D2     8    D1     9    D0    11
VSYNC  6    HREF   7    PCLK  13    PWDN/RESET not connected
```

### RS-485 / Modbus (UART1) — defaults, change in `menuconfig`

```
GPIO47 ──> DI    (transceiver, e.g. MAX3485 / SP3485)
GPIO21 <── RO
GPIO14 ──> DE + RE   (tied together; driven automatically in half-duplex mode)
3V3, GND, and a 120 Ω termination resistor at each end of the bus
```

Set `Modbus RTS GPIO` to `-1` if you are talking to a plain 3.3 V TTL device
with no transceiver; the driver then stays in normal UART mode.

### UART bridge (UART2)

```
GPIO42 = TX    GPIO41 = RX
```

Everything else that is broken out — GPIO 1, 2, 3, 45, 46, 48 and the SD card
pins when you are not using the card — is available to the GPIO API. The
firmware refuses any pin claimed by the camera, either UART, USB, the console
or the flash/PSRAM bus, and tells you which peripheral holds it.

---

## Web UI

`http://<ip>/` has five tabs: Camera, GPIO, UART, Modbus, Console.

The video feed is served from **port 81** on its own `httpd` instance. This
matters: `esp_http_server` handles one request at a time per instance, and the
stream handler holds its request open for as long as someone is watching. With
a single instance the whole API would freeze while the stream ran.

---

## HTTP API

All responses are JSON unless noted. Errors return
`{"ok":false,"error":"..."}` with a matching HTTP status.

### Camera

```
GET  /capture                     single JPEG
GET  :81/stream                   multipart MJPEG
GET  /api/camera                  sensor name + every control
POST /api/camera                  {"framesize":9,"quality":12,"vflip":1}
```

Control names: `framesize quality brightness contrast saturation sharpness
denoise gainceiling colorbar whitebal gain_ctrl exposure_ctrl hmirror vflip
aec2 awb_gain agc_gain aec_value special_effect wb_mode ae_level dcw bpc wpc
raw_gma lenc`.

`framesize` is the esp32-camera enum: 5 = QVGA, 8 = VGA, 9 = SVGA, 10 = XGA,
11 = HD, 13 = UXGA.

### GPIO

```
GET  /api/gpio                    configured pins and live levels
POST /api/gpio                    {"pin":2,"mode":"out"}
                                  {"pin":2,"level":1}
                                  {"pin":2,"mode":"inpu","save":true}
```

Modes: `in`, `inpu`, `inpd`, `out`, `od`, `off`. `"save":true` writes the
table to NVS so it is restored at the next boot. Outputs are configured as
input-output, so `level` in the response is the pad read back rather than an
echo of what was written.

### UART bridge

```
GET  /api/uart                    drains the RX buffer: {"hex":..,"text":..,"length":..}
POST /api/uart                    {"text":"AT\r\n"}
                                  {"hex":"0103000a0001"}
                                  {"baud":9600}
                                  {"flush":true}
```

The RX buffer keeps the most recent `CONFIG_APP_UART_RX_BUF` bytes (4 KB by
default) and drops the oldest when it wraps — `dropped` in the response counts
how many were lost.

### Modbus RTU master

```
GET  /api/modbus                  poll table with cached values and per-job error counts
POST /api/modbus
  {"action":"read","slave":1,"function":3,"start":0,"count":10}
  {"action":"write","slave":1,"function":6,"start":4,"values":[1234]}
  {"action":"poll","enabled":true}
  {"action":"job","index":0,"slave":1,"function":3,"start":0,"count":10,
   "name":"meter","enabled":true,"save":true}
  {"action":"job","index":0,"enabled":false}        # delete slot 0
```

Function codes: 1 read coils, 2 read discrete inputs, 3 read holding
registers, 4 read input registers, 5 write single coil, 6 write single
register, 15 write multiple coils, 16 write multiple registers.

Coils are exchanged as **one value per bit** (0 or 1) in both directions. The
firmware handles the three payload encodings the stack expects underneath:
packed bits for 1/2/15, a bare `uint16_t` for 6, and the Modbus wire encoding
`0xFF00`/`0x0000` for 5.

Functions 5 and 6 take exactly one value; use 15 or 16 for more.

Up to 8 poll jobs, 64 registers each. Poll jobs are read-only on purpose —
a write on a timer is a bad thing to have on a live plant bus, so writes
always go through an explicit `"action":"write"` call.

A slave that does not answer gives **502** with `ESP_ERR_TIMEOUT`; a malformed
request gives **400**.

### System

```
GET  /api/status                  uptime, heap, PSRAM, wifi, camera, uart, modbus, ble
POST /api/wifi                    {"ssid":"...","password":"..."}   (effective next boot)
POST /api/cmd                     body is a raw command line, reply is text/plain
```

---

## BLE

The board advertises as `ESP32S3-CAM` with the **Nordic UART Service**, so any
generic "BLE UART" or "BLE terminal" phone app can drive it — useful for
commissioning a board that has no Wi-Fi yet.

```
Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX       6E400002-...   write    one command per write
TX       6E400003-...   notify   reply text, chunked to the negotiated MTU
```

The service UUID is advertised in the scan response, not the advertisement —
a 128-bit UUID plus the device name does not fit in 31 bytes.

### Command set

Identical on BLE and `POST /api/cmd`:

```
help
status
reboot
wifi <ssid> <password>
gpio list | mode <pin> <in|inpu|inpd|out|od|off> | set <pin> <0|1> | get <pin> | save
uart baud <n> | send <text> | read | monitor <on|off>
mb read <slave> <fn> <start> <count>
mb write <slave> <fn> <start> <val...>
mb poll <on|off> | mb jobs
cam <control> [value]
```

`uart monitor on` mirrors everything arriving on the UART bridge to the BLE
notify channel — a wireless serial sniffer.

Commands are queued and executed on a worker task, never on the NimBLE host
task, because a Modbus transaction can block for the response timeout.

---

## Layout

```
components/       vendored third-party code, committed for offline builds
  esp32-camera/
  esp-modbus/
tools/
  vendor_components.ps1   regenerates the above (needs network)
main/
  main.c          bring-up order
  app_camera.c    sensor init + named control get/set
  app_httpd.c     both httpd instances, MJPEG, the JSON API
  app_wifi.c      STA with SoftAP fallback, NVS credentials
  app_gpio.c      pin table, reserved-pin guard, NVS persistence
  app_uart.c      UART2 bridge, RX ring buffer
  app_modbus.c    RTU master, poll loop, one-shot transactions
  app_cmd.c       text console shared by BLE and HTTP
  app_ble.c       NimBLE peripheral + Nordic UART Service
  www/index.html  the UI, embedded in the binary
```

Peripherals come up before the network in `app_main`: if the camera ribbon is
loose or the Wi-Fi is down, the board still drives its outputs and talks to the
field bus.

---

## Notes and limits

- Modbus TCP and ASCII are compiled out (`CONFIG_FMB_COMM_MODE_TCP_EN=n`,
  `CONFIG_FMB_COMM_MODE_ASCII_EN=n`) — this gateway is an RTU master only.
  Re-enable either in `sdkconfig.defaults` if you extend it.
- Wi-Fi power save is disabled (`WIFI_PS_NONE`); it costs several frames per
  second on the stream. Turn it back on in `app_wifi.c` if you care more about
  current draw than frame rate.
- Wi-Fi and BLE share one radio. `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` is on;
  expect the stream to lose some frames while a BLE client is connected.
- The HTTP API has **no authentication**. Put this on a segregated network,
  or add auth before it goes anywhere untrusted.
