#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
mkdir -p "$LV2_DIR"
ln -sfn "$ROOT/build/generative_arp.lv2" "$LV2_DIR/generative_arp.lv2"
echo "Installed. Restart Zynthian: systemctl restart zynthian"
