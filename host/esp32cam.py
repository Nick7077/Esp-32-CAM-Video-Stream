"""
Frame sources for the ESP32-S3-CAM firmware in ../firmware.

    from esp32cam import WifiCamera, UsbCamera

    cam = UdpCamera("192.168.1.42").start()       # or WifiCamera(...) / UsbCamera("COM5")
    while True:
        frame = cam.read(timeout=2)               # newest BGR numpy array, or None
        ...

Both classes run a background thread that keeps only the *newest* decoded frame.
That is what you want for AI / robotics: if your model takes 80 ms, you process
the latest image instead of falling further and further behind a queue.
"""

from __future__ import annotations

import json
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request
from typing import Callable, Optional

import cv2
import numpy as np

__all__ = ["WifiCamera", "UdpCamera", "UsbCamera", "find_usb_port"]


class _FrameSource:
    """Shared latest-frame logic. Subclasses implement _run() and set()."""

    def __init__(self) -> None:
        self._cond = threading.Condition()
        self._frame: Optional[np.ndarray] = None
        self._jpeg: bytes = b""
        self._seq = 0            # increments on every decoded frame
        self._last_read = 0      # seq handed out by the last read()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.fps = 0.0           # smoothed receive rate
        self.frame_time = 0.0    # time.time() when the newest frame arrived
        self.device_ts_ms = 0    # camera timestamp (ms since boot) if provided
        self.error: Optional[str] = None
        self._t_prev = 0.0

    # -- public API ---------------------------------------------------------
    def start(self):
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name=type(self).__name__, daemon=True)
        self._thread.start()
        return self

    def read(self, timeout: Optional[float] = None) -> Optional[np.ndarray]:
        """Block until a frame newer than the last one returned is available.
        Returns a BGR image, or None on timeout."""
        with self._cond:
            if not self._cond.wait_for(lambda: self._seq > self._last_read or self._stop.is_set(), timeout):
                return None
            self._last_read = self._seq
            return self._frame

    def latest(self) -> Optional[np.ndarray]:
        """Newest frame without waiting (may be the same one as last time)."""
        with self._cond:
            return self._frame

    def latest_jpeg(self) -> bytes:
        """Raw JPEG bytes of the newest frame (handy for saving or forwarding)."""
        with self._cond:
            return self._jpeg

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        if self._thread:
            self._thread.join(timeout=3)

    def set(self, var: str, val) -> bool:  # pragma: no cover - overridden
        raise NotImplementedError

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # -- for subclasses -----------------------------------------------------
    def _publish(self, jpeg: bytes, device_ts_ms: int = 0) -> bool:
        if len(jpeg) < 4 or jpeg[:2] != b"\xff\xd8":
            return False  # not a JPEG (corrupted / out of sync)
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            return False
        now = time.time()
        if self._t_prev:
            dt = now - self._t_prev
            if dt > 0:
                self.fps = 0.9 * self.fps + 0.1 * (1.0 / dt) if self.fps else 1.0 / dt
        self._t_prev = now
        with self._cond:
            self._frame = img
            self._jpeg = jpeg
            self._seq += 1
            self.frame_time = now
            self.device_ts_ms = device_ts_ms
            self.error = None
            self._cond.notify_all()
        return True

    def _run(self) -> None:  # pragma: no cover - overridden
        raise NotImplementedError


# ============================================================================
# Wi-Fi: multipart MJPEG from http://<host>:81/stream
# ============================================================================
class WifiCamera(_FrameSource):
    def __init__(self, host: str, stream_port: int = 81, http_port: int = 80, timeout: float = 5.0):
        super().__init__()
        self.host = host
        self.stream_url = f"http://{host}:{stream_port}/stream"
        self.base_url = f"http://{host}" + ("" if http_port == 80 else f":{http_port}")
        self.timeout = timeout

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                with urllib.request.urlopen(self.stream_url, timeout=self.timeout) as resp:
                    self._read_multipart(resp)
            except Exception as e:  # network hiccup, board reboot, ...
                self.error = f"{type(e).__name__}: {e}"
                self._stop.wait(1.0)  # back off, then reconnect

    def _read_multipart(self, resp) -> None:
        while not self._stop.is_set():
            line = resp.readline()
            if not line:
                raise ConnectionError("stream closed by camera")
            if not line.startswith(b"--"):
                continue  # blank line / preamble
            headers = {}
            while True:
                h = resp.readline()
                if not h:
                    raise ConnectionError("stream closed by camera")
                h = h.strip()
                if not h:
                    break
                k, _, v = h.partition(b":")
                headers[k.strip().lower()] = v.strip()
            length = int(headers.get(b"content-length", b"0"))
            if length <= 0:
                continue
            jpeg = _read_exact(resp, length)
            ts = headers.get(b"x-timestamp")
            ts_ms = int(float(ts) * 1000) if ts else 0
            self._publish(jpeg, ts_ms)

    # -- control API (port 80) ---------------------------------------------
    def set(self, var: str, val) -> bool:
        q = urllib.parse.urlencode({"var": var, "val": val})
        try:
            with urllib.request.urlopen(f"{self.base_url}/control?{q}", timeout=self.timeout) as r:
                return r.status == 200
        except Exception:
            return False

    def status(self) -> dict:
        with urllib.request.urlopen(f"{self.base_url}/status", timeout=self.timeout) as r:
            return json.loads(r.read())

    def capture(self) -> bytes:
        """Grab one JPEG via /capture (independent of the stream)."""
        with urllib.request.urlopen(f"{self.base_url}/capture", timeout=self.timeout) as r:
            return r.read()


def _read_exact(f, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            raise ConnectionError("stream ended mid-frame")
        buf += chunk
    return bytes(buf)


# ============================================================================
# Wi-Fi, low latency: chunked JPEGs over UDP (firmware/src/udp_stream.cpp)
#   header "<2sBBIIIHH": magic "E3", version, session, frame_id, frame_len,
#                         timestamp_ms, chunk_index, chunk_count
# ============================================================================
UDP_HDR = struct.Struct("<2sBBIIIHH")
UDP_MAGIC = b"E3"


class UdpCamera(WifiCamera):
    """Wi-Fi video over UDP. A lost packet drops just that frame instead of
    stalling the stream (which is what TCP/MJPEG does on a flaky link).
    Settings/status/capture still go over HTTP, inherited from WifiCamera."""

    def __init__(self, host: str, udp_port: int = 5005, http_port: int = 80, timeout: float = 5.0):
        super().__init__(host, http_port=http_port, timeout=timeout)
        self.udp_port = udp_port
        self.frames_ok = 0
        self.frames_dropped = 0   # frames that never arrived complete

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._receive()
            except Exception as e:  # e.g. host name not resolvable yet
                self.error = f"{type(e).__name__}: {e}"
                self._stop.wait(1.0)

    def _receive(self) -> None:
        target = (socket.gethostbyname(self.host), self.udp_port)  # resolve once (.local lookups are slow)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # Big receive buffer so bursts aren't dropped by the OS (Windows default is tiny).
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind(("", 0))
            sock.settimeout(0.2)
            asm = FrameAssembler(self._on_frame)
            next_sub = 0.0
            last_packet = time.monotonic()
            while not self._stop.is_set():
                now = time.monotonic()
                if now >= next_sub:
                    sock.sendto(b"SUB", target)  # subscribe / keepalive
                    next_sub = now + 0.5
                try:
                    data, _ = sock.recvfrom(2048)
                except socket.timeout:
                    if now - last_packet > 3:
                        self.error = (f"no UDP video from {target[0]}:{target[1]} - check the IP, that the "
                                      "board runs the latest firmware, and allow Python through the firewall")
                    continue
                except ConnectionResetError:
                    continue  # Windows reports ICMP 'port unreachable' this way (board rebooting)
                last_packet = time.monotonic()
                asm.feed(data)
                self.frames_dropped = asm.dropped
        finally:
            sock.close()

    def _on_frame(self, jpeg: bytes, ts_ms: int) -> None:
        if self._publish(jpeg, ts_ms):
            self.frames_ok += 1


class FrameAssembler:
    """Reassembles UDP chunks into JPEGs. Keeps only frames newer than the last
    completed one; anything older that never completed counts as dropped."""

    MAX_PENDING = 6

    def __init__(self, on_frame: Callable[[bytes, int], object]):
        self.on_frame = on_frame
        self.pending: dict = {}   # frame_id -> [chunks, received, frame_len, ts]
        self.last_done = -1
        self.last_done_time = 0.0
        self.session = None
        self.dropped = 0

    def feed(self, data: bytes) -> None:
        if len(data) < UDP_HDR.size or data[:2] != UDP_MAGIC:
            return
        _, _ver, session, fid, flen, ts, idx, cnt = UDP_HDR.unpack_from(data)
        if session != self.session:
            self.reset()        # board rebooted (new session byte): frame ids start over
            self.session = session
        elif fid <= self.last_done:
            if time.monotonic() - self.last_done_time < 1.0:
                return          # late packet for a frame we've already moved past
            self.reset()        # nothing new for a while and ids went backwards: start over
        entry = self.pending.get(fid)
        if entry is None:
            if not 0 < cnt <= 4096 or not 0 < flen <= MAX_FRAME:
                return
            entry = self.pending[fid] = [[None] * cnt, 0, flen, ts]
        chunks = entry[0]
        if idx >= len(chunks) or chunks[idx] is not None:
            return
        chunks[idx] = data[UDP_HDR.size:]
        entry[1] += 1
        if entry[1] == len(chunks):
            del self.pending[fid]
            jpeg = b"".join(chunks)
            if len(jpeg) != entry[2]:
                self.dropped += 1
                return
            for k in [k for k in self.pending if k < fid]:
                del self.pending[k]  # older frames can't complete usefully any more
            if self.last_done >= 0:
                self.dropped += fid - self.last_done - 1  # ids we skipped never arrived whole
            self.last_done = fid
            self.last_done_time = time.monotonic()
            self.on_frame(jpeg, entry[3])
        elif len(self.pending) > self.MAX_PENDING:
            oldest = min(self.pending)
            del self.pending[oldest]

    def reset(self) -> None:
        self.pending.clear()
        self.last_done = -1


# ============================================================================
# USB: framed JPEGs over the native USB-C port
#   frame = A5 5A C3 3C | uint32 len (LE) | uint32 timestamp_ms (LE) | JPEG
#   anything else on the wire is text (replies/logs), one message per line
# ============================================================================
FRAME_MAGIC = b"\xa5\x5a\xc3\x3c"
HEADER_LEN = 12
MAX_FRAME = 4 * 1024 * 1024
ESPRESSIF_VID = 0x303A  # native USB (USB-Serial/JTAG)


def find_usb_port() -> Optional[str]:
    """Best guess at the ESP32-S3's native USB port (prefers Espressif's VID)."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def _describe_ports() -> str:
    """Human-readable list of serial ports, for error messages."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    if not ports:
        return "No serial ports found at all - check the cable (charge-only cables are common)."
    lines = ["Serial ports on this computer:"]
    for p in ports:
        vidpid = f"VID {p.vid:04X} PID {p.pid:04X}" if p.vid is not None else "not USB"
        lines.append(f"  {p.device:8s} {p.description}  [{vidpid}]")
    return "\n".join(lines)


class UsbCamera(_FrameSource):
    def __init__(self, port: Optional[str] = None, on_text: Optional[Callable[[str], None]] = None):
        super().__init__()
        self._demux: Optional["FrameDemuxer"] = None
        self._dropped_before = 0  # from earlier connections
        self.port = port or find_usb_port()
        if not self.port:
            raise RuntimeError(
                "No ESP32-S3 native USB port found (looking for USB VID 303A).\n"
                "Plug into the board's port labelled USB (not COM/UART), or pass the port explicitly,\n"
                "e.g.  python viewer.py usb COM3\n\n" + _describe_ports()
            )
        self.on_text = on_text or (lambda s: print(f"[esp32] {s}"))
        self.last_status: Optional[dict] = None
        self._ser = None
        self._wlock = threading.Lock()
        self._settings: dict = {}  # re-applied on every (re)connect

    def _open(self):
        import serial

        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200  # ignored by native USB, required by pyserial
        s.timeout = 0.2
        # Keep DTR/RTS low: toggling them on the S3's USB-Serial/JTAG can reset the chip.
        s.dtr = False
        s.rts = False
        s.open()
        return s

    def send(self, line: str) -> None:
        """Send a raw command line (e.g. 'status', 'set framesize SVGA', or your own)."""
        with self._wlock:
            if self._ser:
                self._ser.write((line.strip() + "\n").encode())

    @property
    def frames_dropped(self) -> int:
        """Frames that arrived damaged (cut short or corrupted on the wire) and were skipped."""
        return self._dropped_before + (self._demux.bad_frames if self._demux else 0)

    def set(self, var: str, val) -> bool:
        """Queue/apply a camera setting. Safe to call before the port is open;
        the reply ("ok ..." / "err ...") arrives via on_text."""
        self._settings[var] = val
        self.send(f"set {var} {val}")
        return True

    def stop(self) -> None:
        try:
            self.send("stream off")
        except Exception:
            pass
        super().stop()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._ser = self._open()
                self.send("")            # flush any partial line on the device
                self.send("stream off")
                for var, val in list(self._settings.items()):
                    self.send(f"set {var} {val}")
                self.send("status")
                self.send("stream on")
                self._pump()
            except Exception as e:       # unplugged, reset, port busy...
                self.error = f"{type(e).__name__}: {e}"
                self._stop.wait(1.0)
            finally:
                if self._ser:
                    try:
                        self._ser.close()
                    except Exception:
                        pass
                    self._ser = None

    def _pump(self) -> None:
        # Reading must never stall: if the host stops draining the port for even a
        # few ms (JPEG decode, GUI, GIL), the ESP32's USB buffer fills and it drops
        # bytes mid-frame. So this thread only reads/demuxes; a second thread decodes
        # (keeping just the newest pending frame).
        pending = {"jpeg": None, "ts": 0}
        cond = threading.Condition()

        def on_frame(jpeg: bytes, ts: int) -> None:
            with cond:
                pending["jpeg"], pending["ts"] = jpeg, ts   # overwrite: drop stale frames
                cond.notify()

        def decoder() -> None:
            while not self._stop.is_set() and not done.is_set():
                with cond:
                    if not cond.wait_for(lambda: pending["jpeg"] is not None, 0.2):
                        continue
                    jpeg, ts = pending["jpeg"], pending["ts"]
                    pending["jpeg"] = None
                self._publish(jpeg, ts)

        done = threading.Event()
        dec = threading.Thread(target=decoder, name="UsbDecode", daemon=True)
        dec.start()
        if self._demux:
            self._dropped_before += self._demux.bad_frames
        demux = self._demux = FrameDemuxer(on_frame, self._handle_text)
        last_rx = time.monotonic()
        last_kick = 0.0
        try:
            while not self._stop.is_set():
                data = self._ser.read(self._ser.in_waiting or 1)
                now = time.monotonic()
                if data:
                    last_rx = now
                    demux.feed(data)
                elif now - last_rx > 2.0 and now - last_kick > 2.0:
                    # Video went quiet: make sure the board is (still) streaming.
                    last_kick = now
                    self.send("stream on")
        finally:
            done.set()
            dec.join(timeout=1)

    def _handle_text(self, line: str) -> None:
        if line.startswith("{"):
            try:
                self.last_status = json.loads(line)
            except ValueError:
                pass
        self.on_text(line)


_PRINTABLE = bytes(range(32, 127)) + b"\t\r"


def _looks_like_text(raw: bytes) -> bool:
    """True if at least 95% of the bytes are printable ASCII (firmware messages are)."""
    junk = len(raw.translate(None, _PRINTABLE))
    return junk <= len(raw) // 20


class FrameDemuxer:
    """Splits the USB byte stream into JPEG frames and text lines.
    Kept separate from the serial code so it can be unit-tested.

    If bytes go missing on the wire, a frame's declared length runs into the next
    frame, and the rest of that frame would otherwise be treated as text (a screenful
    of binary garbage). So: a frame whose bytes contain the next header is cut short
    and is dropped (we resync on that header), frames must start with the JPEG SOI
    and end with EOI, and binary junk between frames is counted, never printed."""

    def __init__(self, on_frame: Callable[[bytes, int], object], on_text: Callable[[str], None]):
        self.buf = bytearray()
        self.text = bytearray()
        self.on_frame = on_frame
        self.on_text = on_text
        self.bad_frames = 0   # frames dropped as damaged
        self.junk_bytes = 0   # non-text bytes found between frames

    def feed(self, data: bytes) -> None:
        self.buf += data
        while True:
            i = self.buf.find(FRAME_MAGIC)
            if i < 0:
                # No header yet. Everything except a possible partial magic at the end is text.
                keep = next((k for k in range(min(3, len(self.buf)), 0, -1)
                             if self.buf.endswith(FRAME_MAGIC[:k])), 0)
                n = len(self.buf) - keep
                if n:
                    self._text(self.buf[:n])
                    del self.buf[:n]
                return
            if i:
                self._text(self.buf[:i])
                del self.buf[:i]
            if self.text:
                # The firmware never splits a message around a frame, so an unfinished
                # line at a frame boundary is leftover junk; don't let it swallow the next message.
                self.junk_bytes += len(self.text)
                self.text.clear()
            if len(self.buf) < HEADER_LEN:
                return
            length, ts = struct.unpack_from("<II", self.buf, 4)
            if not 0 < length <= MAX_FRAME:
                del self.buf[:1]  # false magic: skip a byte and resync
                continue
            if len(self.buf) < HEADER_LEN + length:
                return  # wait for the rest of the frame
            j = self.buf.find(FRAME_MAGIC, HEADER_LEN, HEADER_LEN + length)
            if j >= 0:
                # The next frame's header is inside this frame: bytes were lost, so
                # this one is incomplete. Drop it and resync on that header.
                self.bad_frames += 1
                del self.buf[:j]
                continue
            jpeg = bytes(self.buf[HEADER_LEN:HEADER_LEN + length])
            del self.buf[:HEADER_LEN + length]
            if jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
                self.bad_frames += 1  # corrupted in transit
                continue
            self.on_frame(jpeg, ts)

    def _text(self, chunk: bytes) -> None:
        self.text += chunk
        while b"\n" in self.text:
            line, _, rest = self.text.partition(b"\n")
            self.text = bytearray(rest)
            if not _looks_like_text(line):
                self.junk_bytes += len(line)  # leftovers of a damaged frame, not a message
                continue
            s = line.decode("utf-8", "replace").strip()
            if s:
                self.on_text(s)
        if len(self.text) > 4096:  # binary junk with no newline; don't grow forever
            self.junk_bytes += len(self.text)
            self.text.clear()
