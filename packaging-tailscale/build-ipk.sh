#!/usr/bin/env bash
#
# build-ipk.sh - build the distributable Tailscale-agent .ipk for webOS.
#
# Produces org.webosarchive.tailscale_<ver>_all.ipk: a small "info" app that
# bundles the agent payload, plus postinst/prerm that install the agent into
# /usr/lib/vpn/agents/tailscale/ as root.
#
# IMPORTANT: the resulting .ipk must be installed with Preware or WebOS Quick
# Install (which run postinst as root). palm-install does NOT run postinst.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/org.webosarchive.tailscale"
OUT="$HERE/dist"

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
# ships the current agent .so + tailscaled + runner + manifest.
A="$HERE/../agent-tailscale"
[ -f "$A/libVpnTailscaleAgent.so" ] || { echo "run 'make' in agent-tailscale first" >&2; exit 1; }
[ -f "$A/tailscaled" ] || { echo "missing agent-tailscale/tailscaled (see agent-tailscale/BUILD.md)" >&2; exit 1; }
mkdir -p "$APP/agent/icons"
cp "$A/libVpnTailscaleAgent.so" "$A/vpn-plugin-info.json" "$A/tailscaled" "$APP/agent/"
cp "$A/scripts/tailscale-run"   "$APP/agent/"
cp "$A/icons/tailscale-small.png" "$APP/agent/icons/"
echo ">> synced agent payload from agent-tailscale/"

rm -rf "$OUT"; mkdir -p "$OUT"

echo ">> palm-package $APP"
"$PALM_PACKAGE" --outdir "$OUT" "$APP"

IPK="$(ls -t "$OUT"/*.ipk | head -1)"
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

sed -i \
  -e 's|^Maintainer:.*|Maintainer: webOS Archive community <https://github.com/webOSArchive>|' \
  -e 's|^Description:.*|Description: Tailscale (WireGuard) client for the built-in webOS VPN manager. Adds a "Tailscale" option to Settings -> VPN, with NAT traversal, exit-node support and MagicDNS.|' \
  "$WORK/ctrl/control"
( cd "$WORK/ctrl" && tar --owner=0 --group=0 -czf "$WORK/control.tar.gz" . )
( cd "$WORK" && ar rc repacked.ipk debian-binary control.tar.gz data.tar.gz )
mv "$WORK/repacked.ipk" "$IPK"

echo ">> injected postinst + prerm"
echo ">> verifying:"
echo "   members: $(ar t "$IPK" | tr '\n' ' ')"
echo "   control: $(tar -tzf <(ar p "$IPK" control.tar.gz) | tr '\n' ' ')"
echo "   size:    $(du -h "$IPK" | cut -f1)"
echo ""
echo "Built: $IPK"
echo "Install via Preware or WebOS Quick Install (NOT palm-install)."
