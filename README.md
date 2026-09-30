# ESP32-S3-CAM baseline streamer

Baseline video streaming for the dual-USB-C **ESP32-S3-CAM** (ESP32-S3-WROOM-1 **N16R8**, **OV3660**).
It's meant as a starting point for later AI and robotics work.

The camera streams **over Wi-Fi and USB at the same time**:

| Path | How | Good for |
|---|---|---|
| **Wi-Fi** | MJPEG at `http://<ip>:81/stream` plus a web UI at `http://<ip>/` | Untethered robots, viewing from any browser |
| **USB** | Framed JPEGs over the native USB-C port, plus a text command channel | Bench work, no network, lowest jitter |

`host/viewer.py` reads from either path and gives you an OpenCV window with a `process(frame)` hook. That hook is where AI or robotics code goes.

```
firmware/                PlatformIO project (Arduino-ESP32 core 3.x)
  platformio.ini         N16R8 flash/PSRAM settings
  include/config.h       Wi-Fi fallback AP, default resolution/quality, ports
  include/secrets.h      <- you create this (Wi-Fi name/password)
  include/camera_pins.h  OV3660 wiring (ESP32-S3-EYE pinout)
  src/main.cpp           setup(); loop() is left free for your code
  src/camera.cpp         init + runtime settings (framesize, quality, ...)
  src/web_server.cpp     HTTP UI / API / MJPEG stream
  src/usb_link.cpp       USB frame streaming + command parser
  src/net.cpp            Wi-Fi station -> access-point fallback, mDNS, status JSON
host/
  esp32cam.py            WifiCamera / UsbCamera classes (reusable in your own scripts)
  viewer.py              live viewer
```

---

## 1. Flash the firmware

1. Install **VS Code** and the **PlatformIO IDE** extension.
2. Open the `firmware` folder in VS Code (*PlatformIO → Open Project*).
3. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your Wi-Fi name and password.
   This step is optional. If you skip it, or the board can't join, the board creates its own network: **`ESP32S3-CAM`**, password `esp32cam`, at `192.168.4.1`.
4. Plug into the USB-C port labelled **USB**. That's the native port. The other one, labelled **COM/UART**, goes through a CH340/CH343 USB-serial chip. Either port can upload; only **USB** carries USB video.
   Not sure which is which? In Device Manager the native port shows up as *USB Serial Device* / *USB JTAG/serial debug unit*, and the other as *USB-SERIAL CH340* (or CH343).
5. Click **Upload** (→). The first build downloads the ESP32 toolchain, so it takes a few minutes.
   If the upload can't connect, hold **BOOT**, tap **RST**, release **BOOT**, then upload again.
6. Click **Serial Monitor** (🔌) and press **RST**. You should see:
   ```
   camera ok
   ready  UI: http://192.168.1.42/   stream: http://192.168.1.42:81/stream
   ```
   Missed it? Type `status` and press Enter. The board replies with its IP and camera settings.

## 2. View the stream

**Browser:** open `http://<ip>/`, or `http://esp32cam.local/` on most networks. The page has a live view, a resolution menu, a quality slider and a snapshot button.

**Python viewer** (Windows shown; macOS/Linux use `source .venv/bin/activate`):

```bat
cd host
py -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt

python viewer.py udp 192.168.1.42           :: Wi-Fi, low latency (use this one for Wi-Fi)
python viewer.py wifi 192.168.1.42          :: Wi-Fi, MJPEG over HTTP (same stream as the browser)
python viewer.py usb                        :: USB (auto-detects the native port)
python viewer.py usb COM7 --framesize SVGA  :: pick port / resolution
```

Keys: `q` quit · `s` save snapshot (original JPEG → `snapshots/`) · `f` cycle resolution · `+`/`-` quality

> **USB viewer:** close the PlatformIO Serial Monitor first. Only one program can hold the COM port at a time.
> **Wi-Fi stream:** only one `/stream` client at a time (browser *or* Python). `/capture` and `/status` always work.
> **UDP:** the first run may pop up a Windows Firewall prompt for Python. Allow it on *Private* networks.

### Getting smooth video over Wi-Fi

1. **Use `udp` mode, not `wifi`.** MJPEG runs over TCP, so one lost packet stalls everything behind it (freeze, then a burst).
   Over UDP, a lost packet only drops that one frame. The HUD shows how many frames were dropped.
2. **Shrink the frames.** Try `--framesize QVGA` or `HVGA` with `--quality 15` to `20`. Smaller frames mean fewer packets per frame, so fewer chances for one to go missing.
3. **Check the signal.** `http://<ip>/status` shows `rssi`. -60 dBm or better is good; -70 or worse will stutter whatever you do.
   It also shows `udp.fps` / `udp.dropped` as the board sees them.
4. **Take the PC off Wi-Fi** (Ethernet or 5 GHz), and give the camera decent light. In dim light, auto-exposure lowers the frame rate.

## 3. Use it in your own code

```python
from esp32cam import WifiCamera, UsbCamera
import cv2

cam = WifiCamera("192.168.1.42").start()     # or UsbCamera().start()
cam.set("framesize", "VGA")
while True:
    frame = cam.read(timeout=2)              # newest BGR numpy frame, never a stale queued one
    if frame is None:
        continue
    ...                                      # your model / controller here
```

Both classes reconnect on their own if the board resets or Wi-Fi drops.

### AI example (object detection)

```bash
pip install ultralytics
```

```python
# in viewer.py
from ultralytics import YOLO
model = YOLO("yolo11n.pt")

def process(frame):
    return model(frame, verbose=False)[0].plot()
```

### Robotics hooks already in place

- **`loop()` is empty.** Streaming runs in its own tasks, so motor and sensor code can live in `loop()`. Print with `usbPrintf()`, not `Serial.print()`. It can't corrupt a USB frame.
- **Custom USB commands:** set `usbCustomCommand` in `setup()`. There's an example in `main.cpp` (`drive <l> <r>`). From Python, send them with `cam.send("drive 100 100")`.
- **Free GPIOs** are listed at the bottom of `camera_pins.h`. Leave GPIO 26–37 alone; the octal PSRAM and flash use them.

---

## Reference

### HTTP API (Wi-Fi)

| URL | Returns |
|---|---|
| `http://<ip>/` | Viewer page |
| `http://<ip>:81/stream` | `multipart/x-mixed-replace` MJPEG. Each part has `Content-Length` and `X-Timestamp`. Also opens in VLC or `cv2.VideoCapture` |
| `http://<ip>/capture` | One JPEG |
| `http://<ip>/status` | JSON: mode, ip, rssi, heap/PSRAM, camera settings |
| `http://<ip>/control?var=framesize&val=SVGA` | Change a setting (see below) |

### USB protocol (native port)

Host → board: text lines ending in `\n`.

```
stream on | stream off | snap | status | help
set <var> <val>          e.g.  set framesize HD   set quality 10   set hmirror 1
```

Board → host: text lines (replies such as `ok ...` / `err ...`, the status JSON) mixed with binary frames:

```
A5 5A C3 3C | uint32 length (LE) | uint32 timestamp_ms (LE) | <length bytes of JPEG>
```

If the host stops reading or closes the port, the board pauses streaming by itself. `UsbCamera` sends `stream on` again when it reconnects.

### UDP protocol (Wi-Fi, port 5005)

The PC sends `SUB` to `<ip>:5005` every 0.5 s. The board streams to whoever sent it, until it hasn't heard from them for 3 s.
Each datagram carries a 20-byte header followed by up to 1440 bytes of JPEG:

```
"E3" | u8 version | u8 session | u32 frame_id | u32 frame_len | u32 timestamp_ms | u16 chunk_index | u16 chunk_count
```

See `firmware/src/udp_stream.cpp` and `UdpCamera` / `FrameAssembler` in `host/esp32cam.py`.

### Settings (`set` / `/control`)

`framesize` QVGA · VGA · SVGA · XGA · HD · SXGA · UXGA · FHD · QXGA (OV3660 max is QXGA 2048×1536)
`quality` 4–63 (lower = sharper and bigger frames) · `brightness` `contrast` `saturation` −2…2 ·
`hmirror` `vflip` `awb` `aec` `agc` `aec2` `lenc` `dcw` `bpc` `wpc` 0/1 · `aec_value` 0–1200 · `agc_gain` 0–30 · `gainceiling` 0–6 · `special_effect` 0–6 · `wb_mode` 0–4

VGA at quality 12 is a sensible default for vision models. Higher resolutions lower the frame rate on both links.

### Troubleshooting

| Symptom | Fix |
|---|---|
| Build fails unpacking `esp32-core-...-libs` with `FileNotFoundError` on a very long `...esp_matter\connectedhomeip\...` path (Windows) | Windows' 260-character path limit. In an **admin** PowerShell run `New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force`, restart VS Code, delete `%USERPROFILE%\.platformio\.cache\tmp`, and build again. |
| `camera init failed` | Reseat the camera ribbon (contacts facing the board, latch closed). If that doesn't fix it, your clone may use a different pinout; edit `camera_pins.h`. |
| `psramFound()` false / only QVGA works | Check `board_build.arduino.memory_type = qio_opi` in `platformio.ini`. |
| No text in the Serial Monitor | Text and commands work on either port. Type `status` and press Enter. Early boot messages print before the PC opens the port. |
| Upload fails | Hold BOOT → tap RST → release BOOT, then upload. Or upload via the COM port. |
| USB viewer: "port busy" / no frames | Close the Serial Monitor and any other program using the port. |
| Image upside-down or mirrored | `set vflip 0/1`, `set hmirror 0/1`, or change the defaults in `camera.cpp`. |
| Laggy Wi-Fi | Use `viewer.py udp`, a lower resolution, or a higher `quality` number. Keep the board near the router, or use the external-antenna variant. See *Getting smooth video over Wi-Fi*. |
| `udp` mode: "no UDP video from ..." | Re-flash (UDP needs the latest firmware), check the IP, and allow Python through Windows Firewall. Also check the PC's network is set to *Private*. |
| `esp32cam.local` doesn't resolve | Use the IP from `status`. mDNS doesn't work on every network. |
