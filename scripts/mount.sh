#!/bin/bash
# Usage: scripts/mount.sh <backing-dir> <mountpoint>
set -euo pipefail
BACKING=$(cd "$1" && pwd)
MP=$2
mkdir -p "$MP"
exec mount -F -t worldfs "$BACKING" "$MP"   # a world root; equivalently: world fs mount W<n> <mp>
