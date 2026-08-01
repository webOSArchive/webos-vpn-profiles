#!/usr/bin/env bash
#
# build-ipk.sh - build the distributable OpenVPN-agent .ipk for webOS.
#
# Produces org.webosarchive.openvpn_<ver>_all.ipk: a small "info" app that
# bundles the agent payload, plus postinst/prerm that install the agent into
# /usr/lib/vpn/agents/openvpn/ as root.
#
# IMPORTANT: the resulting .ipk must be installed with Preware or WebOS Quick
# Install (which run postinst as root). palm-install does NOT run postinst.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/org.webosarchive.openvpn"
PKG="org.webosarchive.openvpn"
# Shared output folder at the repo root (the latest .ipk of each agent is kept
# in git there). Because it is shared, everything below is scoped to $PKG --
# never wipe the whole directory or the other agent's package goes with it.
OUT="$HERE/../ipks"

# Locate palm-package: honour $PALM_PACKAGE, else PATH, else common SDK paths.
find_palm_package() {
    if [ -n "${PALM_PACKAGE:-}" ]; then echo "$PALM_PACKAGE"; return; fi
    if command -v palm-package >/dev/null 2>&1; then command -v palm-package; return; fi
    for p in /opt/PalmSDK/Current/bin/palm-package /opt/PalmSDK/*/bin/palm-package \
             "$HOME/HP webOS/SDK/bin/palm-package"; do
        [ -x "$p" ] && { echo "$p"; return; }
    done
}
PALM_PACKAGE="$(find_palm_package)"
[ -n "$PALM_PACKAGE" ] && [ -x "$PALM_PACKAGE" ] || {
    echo "ERROR: palm-package not found." >&2
    echo "  Install the HP webOS SDK, or set PALM_PACKAGE=/path/to/palm-package" >&2
    exit 1
}
echo ">> using palm-package: $PALM_PACKAGE"

# Sync the agent payload from the freshly-built artifacts so the .ipk always
# ships the current agent .so + openvpn binary + up-script + manifest.
A="$HERE/../agent-openvpn"
[ -f "$A/libVpnOpenvpnAgent.so" ] && [ -f "$A/openvpn" ] \
  || { echo "build agent-openvpn first (need libVpnOpenvpnAgent.so + openvpn)" >&2; exit 1; }
mkdir -p "$APP/agent/icons"
cp "$A/libVpnOpenvpnAgent.so" "$A/openvpn" "$A/vpn-plugin-info.json" "$APP/agent/"
cp "$A/scripts/openvpn-up" "$APP/agent/"
echo ">> synced agent payload from agent-openvpn/"

mkdir -p "$OUT"
# Scoped cleanup: drop only THIS package's previous build, never the folder.
rm -f "$OUT/$PKG"_*.ipk

echo ">> palm-package $APP"
"$PALM_PACKAGE" --outdir "$OUT" "$APP"

IPK="$(ls -t "$OUT/$PKG"_*.ipk | head -1)"
echo ">> base package: $(basename "$IPK")"

# --- inject postinst/prerm into control.tar.gz ---
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
( cd "$WORK" && ar x "$IPK" )
mkdir -p "$WORK/ctrl"
tar -xzf "$WORK/control.tar.gz" -C "$WORK/ctrl"
cp "$HERE/postinst" "$WORK/ctrl/postinst"
cp "$HERE/prerm"    "$WORK/ctrl/prerm"
chmod 755 "$WORK/ctrl/postinst" "$WORK/ctrl/prerm"

# Tidy the palm-package default control metadata.
sed -i \
  -e 's|^Maintainer:.*|Maintainer: webOS Archive community <https://github.com/webOSArchive>|' \
  -e 's|^Description:.*|Description: OpenVPN client for the built-in webOS VPN manager (TLS 1.3 / AES-256-GCM via OpenSSL 1.1.1w). Adds an "OpenVPN" option to Settings -> VPN.|' \
  "$WORK/ctrl/control"
( cd "$WORK/ctrl" && tar --owner=0 --group=0 -czf "$WORK/control.tar.gz" . )
( cd "$WORK" && ar rc repacked.ipk debian-binary control.tar.gz data.tar.gz )
mv "$WORK/repacked.ipk" "$IPK"

echo ">> injected postinst + prerm"
echo ">> verifying:"
echo "   members: $(ar t "$IPK" | tr '\n' ' ')"
echo "   control: $(tar -tzf <(ar p "$IPK" control.tar.gz) | tr '\n' ' ')"
echo ""
echo "Built: $IPK"
echo "Install via Preware or WebOS Quick Install (NOT palm-install)."
