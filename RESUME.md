# RESUME — webOS OpenVPN agent

## 🎉 2026‑07‑13 — FULL TUNNEL CONFIRMED WORKING against a live PiVPN

Connected a real HP TouchPad to the user's PiVPN through the stock Settings UI and
verified traffic end-to-end:
- `tun0 = 10.87.26.2`, state **connected**, `Initialization Sequence Completed`.
- Server cert verified (Easy-RSA CA), **AES-256-GCM** data cipher, tls-crypt-v2.
- **Internet through the tunnel works** (`ping 1.1.1.1 / 8.8.8.8` 0% loss) and
  **DNS names resolve** (`ping cloudflare.com` OK).
- Two Pi-side NAT/forwarding issues were the last blocker; the user fixed them
  with `pivpn -d`. Nothing device-side remained.

**Two fixes made during this test** (on top of the four in the earlier 2026‑07‑13
block below), both deployed:
1. **VPN Server field now OVERRIDES the .ovpn's `remote`.** PiVPN bakes the
   *public* IP into the .ovpn, which won't reach the Pi from inside the LAN (no
   NAT hairpin) → `TLS key negotiation failed`. write_config now emits `remote
   <vpnHost> <port> <proto>` FIRST (connection-profile #0), so entering the Pi's
   LAN IP (or a DDNS name) in the UI wins. Same .ovpn works on-LAN or remote.
2. **`persist-key` + `persist-tun` in the import wrapper.** On a SIGUSR1
   TLS-restart openvpn re-decrypts the key and, on a non-tty, fatally can't ask
   for the passphrase. persist-key keeps the decrypted key across restarts.

**Post-connect cleanup (done):** lowered openvpn to `verb 3`, added
`pull-filter ignore "block-outside-dns"` (kills the harmless Options-error), and
**removed a notify→poll→notify feedback loop**: `op_get_connection_details` was
calling `notify_state()` on every poll, which the app answered by polling again
(~127×/sec, flooding the log + host callbacks). It now just ACKs; the daemon
already tracks state from the connect/disconnect `notify_state()` calls. Result:
~2082 → ~56 agent-log lines per connect. Tunnel still verified working after.

**Dev gotcha:** the daemon persists connection state (saveDataForRecovery), so
after you `killall PmVpnDaemon` (needed to reload a rebuilt `.so`) the first
`connect` to a profile it still thinks is "connected" **no-ops** (op_connect
doesn't dispatch). Always `disconnect` that profile first, then `connect`. Only
affects the rebuild/restart loop, not normal UI use.

**DNS hand-off (done — DNS now goes through the tunnel):** webOS resolves via a
local dnsmasq (`apps -> 127.0.0.1`) whose *upstream* is `/tmp/resolv.conf`
(dnsmasq `resolv-file`, normally the LAN DNS -> would bypass the tunnel / leak
when roaming). `openvpn-up` now, on `up`, backs up `/tmp/resolv.conf`, rewrites
it to the VPN-pushed DNS (`dhcp-option DNS`, routes over tun0), and
`killall -HUP dnsmasq` (dnsmasq runs `no-poll`); on `down` it restores the backup
and SIGHUPs again. Verified full cycle on device: connected → upstream
`10.87.26.1` via tun0, names resolve through the tunnel; disconnected → original
upstream restored, backup removed, DNS still works. Leak-free now (traffic +
DNS both tunneled). Caveat: if PmNetConfigManager rewrites `/tmp/resolv.conf` on
a mid-session network change it could revert until the next up; fine for a stable
session, and a deeper fix would push DNS via the host2 `addNetworkInterface`
connection-manager path instead.

**Packaging (done):** `packaging-openvpn/` builds a distributable `.ipk`
(`org.webosarchive.openvpn_1.0.0_all.ipk`) — a small info app (icon +
setup instructions in `index.html`) bundling the agent payload, with
`postinst`/`prerm` (repacked into control.tar.gz by `build-ipk.sh`) that install
the agent into `/usr/lib/vpn/agents/openvpn/` as root and restart the daemon.
Icons resized from `openvpn-480.png` (64/48/256 for the app, 32 for the VPN
list). **Must install via Preware / WebOS Quick Install** (they run postinst as
root); `palm-install` won't. Full lifecycle validated on device: postinst
installs + registers (with `vpnAgentIcon`) + tunnel works; prerm removes cleanly.
Build: `cd packaging-openvpn && ./build-ipk.sh` → `ipks/`.

Remaining (all optional polish): the addNetworkInterface route/DNS integration
(cleaner than the resolv.conf rewrite, but current approach works); hosting the
`.ipk` on a GitHub release / the webOS Archive feed.

---

## Update 2026‑07‑13 — real-Pi testing fixes (pipeline validated against a live PiVPN)

Tested against the user's actual PiVPN server (reachable at 192.168.10.2) with a
real `.ovpn` (`/media/internal/touchpad.ovpn`, encrypted key, tls‑crypt‑v2). Four
issues found and fixed — the connect pipeline is now validated end‑to‑end (only
the user's real key passphrase remains to type in):

1. **Name-only connect wasn't loading fields.** The app saves the profile then
   connects **by name only**; the daemon does NOT expand it. Wired up **host11
   (`profileGetPrvDetails`)** — the agent now loads+decrypts the saved profile
   itself. Signature: `host11(name, cb(json,ud), ud)`; confirmed on device
   (`host11=0xbd5c`).
2. **"Connection Failure: undefined" + profile got deleted.** The prompt/soft
   code must be **-7** (silent in *every* scene). We were returning -6, which the
   Add/Configure scenes treat as a real error AND (for a new profile)
   `ConfigureProfile.handleConnectResponse` *deletes the just-saved profile*.
   Fixed: `RSP_NEED_CREDS = -7`.
3. **PiVPN's per-client password encrypts the private KEY**, it's not a login. In
   cert mode the agent now writes it to an askpass file and adds `askpass <file>`
   (was only doing `auth-user-pass` in login mode). Verified: right/wrong
   passphrase → key decrypts / `bad decrypt` as expected.
4. **openvpn's failure output was being lost** (the child-exit watch removed the
   pipe watch before draining). Fixed with `drain_channel()` on exit + close-on-
   unref; failures are now fully logged.

All deployed to the device this session. The user just needs to (re)create the
profile in the UI (Auth = *Certificate (.ovpn)*, Config file =
`/media/internal/touchpad.ovpn`, Password = their `pivpn add` passphrase) and
Connect. Everything below (from 2026‑07‑12) still applies.

---

# RESUME — webOS OpenVPN agent (session 2026‑07‑12)

**Start-here doc for picking the work back up.** Everything below was done and
**validated on the connected HP TouchPad** this session. Skim §1 for status, §6
to test the real tunnel, §7 for what's next.

---

## 1. TL;DR — where things stand

A native **OpenVPN VPN-agent plugin** for legacy webOS (`org.webosarchive.openvpn`)
is **built, installed on the device, and working end-to-end** — inside the stock
Settings → VPN UI, no app/daemon patching.

Confirmed on-device:
- OpenVPN appears in Add Profile → Connection Type (via `getAgents`).
- Selecting it prompts for OpenVPN settings through the stock configure-profile scene.
- Connect spawns a cross-compiled **OpenVPN 2.5.9 linked against OpenSSL 1.1.1w**
  (runs on the TouchPad; `library versions: OpenSSL 1.1.1w`).
- Connection state (connecting/connected/disconnected) flows back to the UI.
- Disconnect SIGTERMs openvpn cleanly — no orphan process.
- **No daemon crashes** across many connect/disconnect cycles.

**Not yet done (polish, not blockers):** DNS/route hand-off to the webOS
connection manager (IP routing works; pushed DNS isn't applied to the system
resolver yet). See §7.

The device currently has the final agent installed and is clean (no test profiles
or leftover processes). Nothing has been committed to git.

---

## 2. What was built this session (files)

All under `/home/jonwise/Projects/webos-vpn-experiments/`:

| Path | What |
|---|---|
| `agent-openvpn/src/openvpn_agent.c` | The agent. Fills the descriptor, captures the host callback table, implements the 5 ops, generates an openvpn config, spawns/monitors openvpn via the GLib main loop. |
| `agent-openvpn/src/webos_vpn_agent_abi.h` | The **fully reverse-engineered ABI** (descriptor, op signature, response cb, host table). Authoritative. |
| `agent-openvpn/vpn-plugin-info.json` | Manifest (`id: org.webosarchive.openvpn`, `type: ["ssl"]`, `plugin: libVpnOpenvpnAgent.so`). |
| `agent-openvpn/scripts/openvpn-up` | openvpn `--up/--down` hook (records state now; DNS/route TODO). |
| `agent-openvpn/Makefile` | Cross-compiles the `.so`. Linaro toolchain + in-tree sysroot. |
| `agent-openvpn/sysroot/usr/include/{json.h,glib_shim.h}` | Hand-written header shims (device strips `/usr/include`). `glib_shim.h` uses **32-bit ARM types** — do not swap for host glib headers (64-bit types would corrupt the ABI). |
| `agent-openvpn/sysroot/usr/lib/` | Device libs pulled via novacom (`libcjson.so`, `libglib-2.0.so.0`) — link targets. (gitignored) |
| `agent-openvpn/libVpnOpenvpnAgent.so` | Prebuilt agent (19 KB). |
| `agent-openvpn/openvpn` | Prebuilt OpenVPN 2.5.9 for the device (438 KB, stripped). |
| `agent-openvpn/install-openvpn-agent.sh` | One-command novacom installer (idempotent). |
| `agent-openvpn/BUILD.md` | Build/architecture guide (updated with the validated ABI). |
| `build-openvpn/openvpn-2.5.9/` | OpenVPN source + build tree (gitignored). |
| `PIVPN-GUIDE.md` | Community guide: PiVPN server + TouchPad client, troubleshooting. |
| `reversing/PmVpnDaemon`, `*.disasm.txt` | Pulled daemon + disassembly (ABI ground truth). |
| `CLAUDE.md` | Project resume notes (has a condensed version of this). |

---

## 3. How it works (architecture in one screen)

```
Settings VPN app  (com.palm.app.vpn, Enyo, unmodified)
        │  luna calls: getAgents / connect / disconnect / addProfile / uiPromptResponse
        ▼
PmVpnDaemon  (/usr/bin/PmVpnDaemon, unmodified)
   • scans /usr/lib/vpn/agents/*/vpn-plugin-info.json  → lists OpenVPN
   • on connect: activatePlugin → dlopen(libVpnOpenvpnAgent.so)
                 → initVpnAgent(desc, host0..host11)  [we fill desc, stash host fns]
   • dispatches ops:  op(token, params_JSON_string, cb)
        │                                   ▲
        ▼                                   │ host0=SendMsgToApp(json)  → launches the form
   libVpnOpenvpnAgent.so  (our agent)       │ host1=Notify(json)        → pushes status
   • op_connect: parse profile → write /tmp/webos-openvpn/client.conf
                 → fork/exec openvpn (LD_LIBRARY_PATH=/usr/lib/ssl11)
                 → g_io_add_watch(pipe) + g_child_watch_add(pid)  [daemon main loop]
                 → cb(token,0,0,NULL) ACK; states via host1
   • op_disconnect: SIGTERM openvpn; on_openvpn_exit → notify "disconnected"
        │
        ▼
   openvpn 2.5.9  →  OpenSSL 1.1.1w (/usr/lib/ssl11)  →  tun0  →  Pi (PiVPN)
```

Import model: the user copies a PiVPN `.ovpn` to the device and sets the
"Config file (.ovpn) path" field; the agent `config`-includes it (its inline
ca/cert/key/tls-crypt + remote are authoritative) and only layers on the up/down
hook.

---

## 4. The ABI, exactly (so you never have to re-derive it)

Recovered from `libVpncAgent.so` **and** `PmVpnDaemon` (both unstripped) and
confirmed by on-device behaviour (agent log prints `host0=0xdb14 host1=0xdbc0`,
matching the daemon symbol table).

- **Entry points** (dlsym'd): `int initVpnAgent(desc, h0..h11)`, `void cleanupVpnAgent(void)`.
  - `initVpnAgent` **MUST return int 0** — `activatePlugin` does `cmp r0,#0; bne <unload>`.
    A `void`/nonzero return makes the daemon dlclose the plugin before any op runs.
- **Descriptor** = first 60 bytes of the daemon's 72-byte plugin struct (daemon owns
  0x3c..0x47; `memset` only 60): `0x00 version=1`, `0x04 id[32]`, `0x28 connect`,
  `0x2c disconnect`, `0x30 get_connection_details`, `0x34 handle_ui_prompt_resp`,
  `0x38 notify_system_change`.
- **Op signature**: `void op(void *token, const char *params_json, cb)`.
  - `params_json` is a **raw JSON string** (daemon g_strdup's the payload) — parse
    with `json_tokener_parse`. **Treating it as a json_object* segfaults the daemon.**
  - Reply once: `cb(token, 0, code, errText)`. `code 0` = success (returnValue true),
    negative = failure, **`-6` = silent** in the app (use after launching the form).
    Connect carries no payload; state comes async via host1.
- **Host callback table** (daemon `.data` @ `0x21988`, passed 3-in-registers + rest
  on stack): `[0] handleLunaServiceSendMsgToAppCb`, `[1] handleLunaServiceNotifyCb`,
  `[2] addNetworkInterface`, `[3] modifyNetworkInterface`, `[4] removeNetworkInterface`,
  `[5] getNetworkInterfaces`, `[6] updateIpTableRules`, `[7..10] (unnamed)`,
  `[11] profileGetPrvDetails`. **host0 and host1 each take one JSON string.**
  They touch the default GMainContext → **main-thread only** (hence the GLib watch design).
- **Prompt envelope** → host0 (becomes the app's `enyo.windowParams`):
  `{vpnAgentGuid, vpnMsgType:"credentials", vpnHost?, vpnProfileName?,
    vpnFormFields:[{id,type,label,value,editable,options?/trueValue,falseValue?}]}`.
  Field types: `textfield|passwordfield|listselector|checkbox`. The app shows the
  configure-profile scene when `vpnFormFields` present (and `popupPrompt` for the
  in-connection modal).
- **Status envelope** → host1: `{vpnProfileName, state}`, state ∈
  `connecting|connected|disconnecting|disconnected|reconnecting`.
- **Profile shapes**: addProfile/connect use
  `{vpnProfileName, vpnAgentGuid, vpnProfile:{vpnFormFields:[...], vpnHost}}`.
  The daemon passes the client's payload verbatim to the op — the **real app sends
  the full profile** on connect, so op_connect gets the fields.
- **Gotchas that cost debug cycles** (all fixed):
  1. `initVpnAgent` returned void → daemon unloaded the plugin.
  2. Op arg1 treated as object not string → SIGSEGV in daemon.
  3. Background pthread calling host1 → race with Disconnect → SIGSEGV.
  Also: Connect requires EULA (`acceptEula`), and Disconnect only dispatches if the
  profile is saved (`addProfile`) so the daemon tracks it active — both automatic in the UI.

---

## 5. Build & install (repeatable)

**Toolchain**: Linaro 4.9.4 `arm-linux-gnueabi` at
`/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi` (softfp, matches device).

**Agent:**
```sh
cd agent-openvpn
make                 # -> libVpnOpenvpnAgent.so   (links device libcjson + libglib-2.0)
make check           # ELF32 ARM v7 softfp, NEEDED libcjson/libglib/libc, exports the 2 entry pts
```
If the sysroot libs are missing (fresh clone), repopulate from the device:
```sh
SR=agent-openvpn/sysroot/usr/lib
for f in libcjson.so libglib-2.0.so.0.1600.6; do novacom get file:///usr/lib/$f > $SR/$f; done
(cd $SR && ln -sf libglib-2.0.so.0.1600.6 libglib-2.0.so.0 && ln -sf libglib-2.0.so.0.1600.6 libglib-2.0.so)
```

**OpenVPN** (already built; only if you need to rebuild):
```sh
cd build-openvpn/openvpn-2.5.9
export CC=/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-gcc
O=/home/jonwise/Projects/OpenSSL-legacyWebOS/openssl-1.1.1w
./configure --host=arm-linux-gnueabi \
  CFLAGS="-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp -O2" \
  OPENSSL_CFLAGS="-I$O/include" OPENSSL_LIBS="-L$O -lssl -lcrypto" \
  IFCONFIG=/sbin/ifconfig ROUTE=/sbin/route NETSTAT=/bin/netstat \
  --disable-lzo --disable-lz4 --disable-plugins --disable-pkcs11 \
  --disable-debug --disable-management --disable-systemd --disable-selinux \
  --disable-dependency-tracking
make -j4
arm-linux-gnueabi-strip src/openvpn/openvpn -o ../../agent-openvpn/openvpn
```

**Install onto the device:**
```sh
cd agent-openvpn && ./install-openvpn-agent.sh
```

---

## 6. Test the real tunnel tomorrow (device is ready)

1. **Pi**: `curl -L https://install.pivpn.io | bash` → choose **OpenVPN**, forward the
   port, then `pivpn add -n touchpad` → gives `~/ovpns/touchpad.ovpn`. (Full detail in
   `PIVPN-GUIDE.md`.)
2. **Copy the .ovpn to the device**, e.g.:
   ```sh
   echo 'mkdir -p /media/internal/vpn' | novacom run file://bin/sh
   novacom put file:///media/internal/vpn/client.ovpn < ~/ovpns/touchpad.ovpn
   ```
3. **On the TouchPad**: Settings → VPN → Add Profile → Connection Type **OpenVPN** →
   enter your host → Next → set **Config file (.ovpn) path** to
   `/media/internal/vpn/client.ovpn` → Save & Connect.
4. **Watch it**:
   ```sh
   echo 'tail -n 200 -f /var/log/webos-openvpn-agent.log' | novacom run file://bin/sh
   ```
   Success = openvpn logs `Initialization Sequence Completed` and the UI flips to
   Connected. Troubleshooting table is in `PIVPN-GUIDE.md`.

Sanity checks any time:
```sh
echo "luna-send -n 1 palm://com.palm.vpn/getAgents '{}'" | novacom run file://bin/sh   # OpenVPN listed?
echo 'LD_LIBRARY_PATH=/usr/lib/ssl11 /usr/lib/vpn/agents/openvpn/openvpn --version' | novacom run file://bin/sh
```

**Handy on-device luna-send test sequence** (bypasses the touchscreen):
```sh
luna-send -n 1 palm://com.palm.vpn/acceptEula '{"vpnAgentGuid":"org.webosarchive.openvpn"}'
luna-send -n 1 palm://com.palm.vpn/addProfile '{"vpnProfileName":"MyPi","vpnAgentGuid":"org.webosarchive.openvpn","vpnProfile":{"vpnHost":"YOURHOST","vpnFormFields":[{"id":"vpnConfigFile","type":"textfield","value":"/media/internal/vpn/client.ovpn"},{"id":"vpnAuthMode","type":"listselector","value":"cert"}]}}'
luna-send -i palm://com.palm.vpn/connect '{"vpnProfileName":"MyPi","vpnAgentGuid":"org.webosarchive.openvpn","vpnProfile":{"vpnHost":"YOURHOST","vpnFormFields":[{"id":"vpnConfigFile","value":"/media/internal/vpn/client.ovpn"},{"id":"vpnAuthMode","value":"cert"}]}}'
# ...
luna-send -n 1 palm://com.palm.vpn/disconnect '{"vpnProfileName":"MyPi"}'
```
(Connect must include the full `vpnProfile` — the daemon forwards the payload as-is.)

---

## 7. Next steps (in priority order)

1. **DNS + route integration with the webOS connection manager.** This is the one
   gap between "tunnel up" and "seamless". Flesh out `scripts/openvpn-up` to call the
   host callbacks we already identified: `host2 addNetworkInterface`,
   `host6 updateIpTableRules` (register tun0 + apply pushed DNS the way
   `vpnc-script-palm` does the `/var/run/resolv.conf` dance). The agent already stashes
   the whole host table in `initVpnAgent` — wire the up-script to signal the agent (or
   have the agent parse `foreign_option_*` and call the host fns directly). Look at
   `reversing/vpnc-script-palm` and `vpnc_get_interface_info` for the contract.
2. **Verify a full real-world connect** to your Pi (the only thing not testable here —
   there's no Pi on the build box). Expect IP routing to work; DNS pending step 1.
3. **`.ipk` packaging** → install to `/media/cryptofs/apps/usr/palm/vpnframework/agents`
   (the daemon already scans that path — see `PmVpnDaemonStarter.sh` env
   `PALM_VPN_INSTALLED_AGENT_PATH`). Makes it installable without novacom / survives better.
4. **UX polish**: icon (manifest `icon` field), better field labels, optional
   on-device `.ovpn` import, cert-only vs cert+login handling.
5. **Ship the guide**: publish `PIVPN-GUIDE.md` for the webOS Archive community.

---

## 8. Environment facts (so tomorrow-you doesn't re-discover them)

- **Device**: HP TouchPad, webOS 3.0.5, kernel 2.6.35, ARMv7-A softfp, `/dev/net/tun` present.
- **novacom** usage: `echo '<cmds>' | novacom run file://bin/sh` (root). `novacom get
  file:///path > local` to pull, `novacom put file:///path < local` to push.
- **VPN daemon**: `/usr/bin/PmVpnDaemon`, on-demand (dbus `Type=dynamic`, launched by
  `PmVpnDaemonStarter.sh` on first luna call, **exits when idle** — a dead `ps` grep is
  NOT a crash; check `/var/log/messages` for `SIGSEGV`/`minicore` to detect real crashes).
  Restart to re-scan agents: `killall PmVpnDaemon`.
- **OpenSSL 1.1.1w** already on device at `/usr/lib/ssl11` (from the
  codepoet80/OpenSSL-legacyWebOS package — a hard dependency of our openvpn binary).
  Source/headers/cross-libs at `/home/jonwise/Projects/OpenSSL-legacyWebOS/openssl-1.1.1w`.
- **Agent install dir on device**: `/usr/lib/vpn/agents/openvpn/`.
- **Agent log on device**: `/var/log/webos-openvpn-agent.log`.
- **Generated openvpn config on device**: `/tmp/webos-openvpn/client.conf`.
- Luna methods: `getAgents, connect, disconnect, getConnectionDetails, getStatus,
  getProfileList, getProfileDetails, addProfile, updateProfile, deleteProfile,
  acceptEula, uiPromptResponse` on `palm://com.palm.vpn/`.

Nothing is committed — `git status` shows the changes. Commit on a branch when ready.
```
git checkout -b openvpn-agent && git add -A && git commit
```
```
