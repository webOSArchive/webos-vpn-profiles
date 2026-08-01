#!/usr/bin/env bash
#
# install-tailscale-agent.sh  --  install the webOS Tailscale VPN-agent onto a
# TouchPad over novacom, from the machine that built it.
#
# Layout (split on purpose):
#   /usr/lib/vpn/agents/tailscale/vpn-plugin-info.json    manifest
#   /usr/lib/vpn/agents/tailscale/libVpnTailscaleAgent.so the agent (~20 KB)
#   /usr/lib/vpn/agents/tailscale/tailscale-run           orchestrator script
#   /usr/lib/vpn/agents/tailscale/tailscale -> ...        CLI symlink (0 bytes)
#   <app dir>/agent/tailscaled                            23 MB static Go binary
#
# The big binary deliberately does NOT go on / (only ~120 MB free there). It
# lives on /media/cryptofs, which has tens of GB. cryptofs is fuse and refuses
# symlinks, but / is ext3 -- hence the CLI symlink lives on / and points at the
# cryptofs binary. tailscaled is a combined binary: argv[0] == "tailscale"
# selects CLI mode, so one copy serves both roles.
#
# Requires: novacom in PATH and a connected device. Re-run any time to update.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
D=/usr/lib/vpn/agents/tailscale
APP=/media/cryptofs/apps/usr/palm/applications/org.webosarchive.tailscale
AGENT="$APP/agent"

need() { [ -f "$HERE/$1" ] || { echo "missing build artifact: $1 (run 'make' first)" >&2; exit 1; }; }
need libVpnTailscaleAgent.so
need vpn-plugin-info.json
need scripts/tailscale-run
need tailscaled

command -v novacom >/dev/null || { echo "novacom not in PATH" >&2; exit 1; }
novacom -l | grep -q . || { echo "no device on novacom (novacom -l is empty)" >&2; exit 1; }

echo ">> checking device prerequisites"
echo 'test -e /dev/net/tun && echo "tun: ok" || echo "tun: MISSING (reboot usually restores it)"
      echo "root fs free: $(df -h / | tail -1 | awk "{print \$4}")"' \
  | novacom run file://bin/sh

echo ">> remounting / read-write and creating directories"
echo "mount -o remount,rw / && mkdir -p $D/icons $AGENT && echo ok" | novacom run file://bin/sh

echo ">> pushing agent files (small, to /)"
novacom put file://$D/vpn-plugin-info.json      < "$HERE/vpn-plugin-info.json"
novacom put file://$D/libVpnTailscaleAgent.so   < "$HERE/libVpnTailscaleAgent.so"
novacom put file://$D/tailscale-run             < "$HERE/scripts/tailscale-run"
novacom put file://$D/icons/tailscale-small.png < "$HERE/icons/tailscale-small.png"

# 23 MB over novacom -- a few seconds. Skip with SKIP_BIN=1 when iterating on the
# agent/script only.
if [ "${SKIP_BIN:-0}" != "1" ]; then
  echo ">> pushing tailscaled (23 MB, to /media/cryptofs)"
  novacom put file://$AGENT/tailscaled          < "$HERE/tailscaled"
else
  echo ">> SKIP_BIN=1: leaving existing tailscaled in place"
fi

echo ">> permissions + CLI symlink"
echo "chmod 644 $D/vpn-plugin-info.json;
      chmod 755 $D/libVpnTailscaleAgent.so $D/tailscale-run $AGENT/tailscaled;
      ln -sf $AGENT/tailscaled $D/tailscale;
      mkdir -p $AGENT/state;
      ls -la $D" | novacom run file://bin/sh

echo ">> restarting VPN daemon (re-scans agents)"
echo "killall PmVpnDaemon 2>/dev/null; sleep 1; echo done" | novacom run file://bin/sh

echo ">> verifying Tailscale is now registered"
echo "luna-send -n 1 palm://com.palm.vpn/getAgents '{}' 2>/dev/null" \
  | novacom run file://bin/sh | tr ',' '\n' | grep -i tailscale || echo "(not listed -- check the log)"

cat <<EOF

Done. On the device:
  Settings -> VPN -> Add Profile -> Connection Type: Tailscale
  Server: controlplane.tailscale.com   (or your headscale URL)
  Then fill in an auth key from the Tailscale admin console
  (Settings -> Keys -> Generate auth key) and Save & Connect.

Logs on device:  tail -f /var/log/webos-tailscale-agent.log
EOF
