"""
Live viewer for the ESP32-S3-CAM - over Wi-Fi or USB.

    python viewer.py udp 192.168.1.42         # Wi-Fi, low latency (recommended for Wi-Fi)
    python viewer.py wifi 192.168.1.42        # Wi-Fi, MJPEG over HTTP (same as the browser)
    python viewer.py usb                      # auto-detects the native USB port
    python viewer.py usb COM5                 # or pick the port yourself

Keys:  q/Esc quit   s save snapshot   f next resolution   +/- JPEG quality

Put your own AI / robotics logic in process() below.
"""

import argparse
import sys
import time
from pathlib import Path

import cv2

from esp32cam import UdpCamera, UsbCamera, WifiCamera

# Used when you run the file with no arguments (e.g. VS Code's Run button).
# Change to e.g. ["udp", "192.168.1.42"] to default to Wi-Fi.
DEFAULT_ARGS = ["usb"]

RESOLUTIONS = ["QVGA", "VGA", "SVGA", "XGA", "HD", "SXGA", "UXGA"]


def process(frame):
    """Your hook. Gets every new BGR frame (numpy HxWx3); return the image to display.

    Examples:
      - Object detection:   return yolo_model(frame, verbose=False)[0].plot()
      - Line following:     threshold -> find contour -> compute steering -> cam.send("drive ...")
      - Save a dataset:     cv2.imwrite(f"data/{time.time():.3f}.jpg", frame)
    """
    return frame


def draw_hud(img, cam, latency_ms):
    h, w = img.shape[:2]
    text = f"{w}x{h}  {cam.fps:4.1f} fps  process() {latency_ms:4.1f} ms"
    if isinstance(cam, UdpCamera):
        text += f"  dropped {cam.frames_dropped}"
    cv2.putText(img, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(img, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (80, 255, 80), 1, cv2.LINE_AA)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    d = sub.add_parser("udp", help="low-latency video over Wi-Fi (UDP)")
    d.add_argument("host", help="IP or hostname of the camera, e.g. 192.168.1.42 or esp32cam.local")
    w = sub.add_parser("wifi", help="MJPEG over Wi-Fi (HTTP, same stream as the browser)")
    w.add_argument("host", help="IP or hostname of the camera, e.g. 192.168.1.42 or esp32cam.local")
    u = sub.add_parser("usb", help="framed JPEG over the native USB-C port")
    u.add_argument("port", nargs="?", help="serial port (COM5, /dev/ttyACM0, /dev/cu.usbmodem...)")
    for p in (d, w, u):
        p.add_argument("--framesize", default="VGA", choices=RESOLUTIONS + ["FHD", "QXGA"])
        p.add_argument("--quality", type=int, default=12, help="JPEG quality 4-63, lower = better")
    args = ap.parse_args(sys.argv[1:] or DEFAULT_ARGS)

    try:
        if args.mode == "udp":
            cam = UdpCamera(args.host)
        elif args.mode == "wifi":
            cam = WifiCamera(args.host)
        else:
            cam = UsbCamera(args.port)
    except RuntimeError as e:
        sys.exit(f"\n{e}")
    cam.start()
    fs_idx = RESOLUTIONS.index(args.framesize) if args.framesize in RESOLUTIONS else 1
    quality = args.quality
    cam.set("framesize", args.framesize)
    cam.set("quality", quality)

    out_dir = Path("snapshots")
    win = f"ESP32-S3-CAM ({args.mode})"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    where = cam.port if args.mode == "usb" else args.host
    print(f"Connecting via {args.mode} ({where})... (q to quit)")
    last_msg = 0.0

    try:
        while True:
            frame = cam.read(timeout=0.5)
            if frame is None:
                if time.time() - last_msg > 3:
                    print("waiting for frames..." + (f" ({cam.error})" if cam.error else ""))
                    last_msg = time.time()
                if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
                    break
                continue

            t0 = time.perf_counter()
            shown = process(frame)
            draw_hud(shown, cam, (time.perf_counter() - t0) * 1000)
            cv2.imshow(win, shown)

            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                break
            if key == ord("s"):
                out_dir.mkdir(exist_ok=True)
                path = out_dir / time.strftime("snap_%Y%m%d_%H%M%S.jpg")
                path.write_bytes(cam.latest_jpeg())  # original JPEG, no re-encode
                print(f"saved {path}")
            elif key == ord("f"):
                fs_idx = (fs_idx + 1) % len(RESOLUTIONS)
                cam.set("framesize", RESOLUTIONS[fs_idx])
            elif key in (ord("+"), ord("=")):
                quality = max(4, quality - 2)  # lower number = better quality
                cam.set("quality", quality)
            elif key == ord("-"):
                quality = min(63, quality + 2)
                cam.set("quality", quality)
    finally:
        cam.stop()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
