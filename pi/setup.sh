#!/bin/bash
# Stack-chan Raspberry Pi Zero W 2 setup (Raspberry Pi OS Bookworm).
# Run as: bash setup.sh
set -e

echo "== Stack-chan Pi setup =="

# 1. ttyd (web terminal the robot displays on its own screen)
if command -v ttyd >/dev/null 2>&1; then
    echo "ttyd already installed"
else
    echo "installing ttyd..."
    sudo apt-get update -qq
    sudo apt-get install -y ttyd || {
        echo "apt ttyd failed, trying github binary..."
        ARCH=$(uname -m)
        case "$ARCH" in
            armv7l|armv6l) URL=https://github.com/tsl0922/ttyd/releases/download/1.7.7/ttyd.armhf ;;
            aarch64)       URL=https://github.com/tsl0922/ttyd/releases/download/1.7.7/ttyd.aarch64 ;;
            *)             URL=https://github.com/tsl0922/ttyd/releases/download/1.7.7/ttyd.x86_64 ;;
        esac
        sudo wget -O /usr/bin/ttyd "$URL"
        sudo chmod +x /usr/bin/ttyd
    }
fi

# 2. agent
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
sudo cp "$SCRIPT_DIR/stackchan_agent.py" /home/pi/stackchan_agent.py
sudo cp "$SCRIPT_DIR/stackchan-agent.service" /etc/systemd/system/
sudo cp "$SCRIPT_DIR/ttyd.service" /etc/systemd/system/

# 3. optional extras (TTS on the Pi, quick exec helpers)
sudo apt-get install -y espeak-ng alsa-utils || true

# 4. enable services
sudo systemctl daemon-reload
sudo systemctl enable --now ttyd
sudo systemctl enable --now stackchan-agent

echo
echo "== Done =="
echo "  ttyd:   http://$(hostname).local:7681  (login pi:stackchan — CHANGE in /etc/systemd/system/ttyd.service)"
echo "  agent:  port 8765 (token 'stackchan' — CHANGE via /etc/systemd/system/stackchan-agent.service)"
echo
echo "On the CoreS3 web UI (Config tab), set:"
echo "  pi_host = $(hostname).local (or the Pi's IP)"
echo "  pi_token = pi:stackchan"
