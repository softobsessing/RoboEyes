#!/bin/sh
# Serve the RoboEyes web app on http://localhost:8000 (localhost counts as a secure context, so
# Web Serial and Web Bluetooth work). Open it in desktop Chrome.
cd "$(dirname "$0")" || exit 1
echo "RoboEyes: open http://localhost:8000/ in Chrome (Ctrl+C to stop)"
exec python3 -m http.server 8000 --bind 127.0.0.1
