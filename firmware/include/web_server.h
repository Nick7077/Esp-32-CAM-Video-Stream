#pragma once

// Port 80:  /  (viewer page)   /capture (one JPEG)   /status (JSON)   /control?var=..&val=..
// Port 81:  /stream  (multipart MJPEG - open in a browser, VLC, OpenCV, or host/viewer.py)
// Note: /stream serves one client at a time.
void webServerStart();
