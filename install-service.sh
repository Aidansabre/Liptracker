#!/bin/sh
# Modified 2026: both tracker IDs, system-manager USB lifecycle, atomic install.
# Run as the normal SteamOS user; sudo is used only for system configuration.
set -eu
dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
user=$(id -un)
if [ "$user" = root ]; then echo 'Run sh install-service.sh as the SteamOS user, without sudo.' >&2; exit 1; fi
case "$user" in *[!a-zA-Z0-9_-]*) echo 'Unsupported username' >&2; exit 1;; esac
case "$HOME" in *'"'*|*'%'*|*'\'*) echo 'Unsupported home-directory path' >&2; exit 1;; esac
getent group video >/dev/null || { echo 'The video group is missing.' >&2; exit 1; }
[ -x "$dir/vft-stream" ] || { echo 'Put the ARM64 binary named vft-stream next to this script.' >&2; exit 1; }
mkdir -p "$HOME/.local/bin"
install -m755 "$dir/vft-stream" "$HOME/.local/bin/vft-stream.new"
mv -f "$HOME/.local/bin/vft-stream.new" "$HOME/.local/bin/vft-stream"

# Migrate the upstream user service; device units belong to the system manager.
systemctl --user disable --now vft-stream.service 2>/dev/null || true
sudo tee /etc/systemd/system/vft-stream.service >/dev/null <<UNIT
[Unit]
Description=Vive Facial Tracker / Focus 3 MJPEG stream
BindsTo=sys-subsystem-usb-vft.device
After=sys-subsystem-usb-vft.device

[Service]
User=$user
SupplementaryGroups=video
ExecStart="$HOME/.local/bin/vft-stream"
Restart=on-failure
RestartSec=2
TimeoutStopSec=10
UNIT
sudo tee /etc/udev/rules.d/70-vive-facial-tracker.rules >/dev/null <<'RULE'
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0bb4", ATTR{idProduct}=="0321", MODE="0660", GROUP="video", TAG+="systemd", ENV{SYSTEMD_ALIAS}="/sys/subsystem/usb/vft", ENV{SYSTEMD_WANTS}+="vft-stream.service"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0bb4", ATTR{idProduct}=="06a1", MODE="0660", GROUP="video", TAG+="systemd", ENV{SYSTEMD_ALIAS}="/sys/subsystem/usb/vft", ENV{SYSTEMD_WANTS}+="vft-stream.service"
RULE
sudo systemctl daemon-reload
sudo udevadm control --reload
sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=0bb4
if systemctl is-active --quiet sys-subsystem-usb-vft.device; then
  sudo systemctl restart vft-stream.service
  systemctl --no-pager status vft-stream.service
else
  echo 'Installed. Plug in the tracker to start vft-stream.'
fi
