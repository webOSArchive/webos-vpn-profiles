# webos-vpn — project context / resume notes

**Goal:** Figure out what VPN protocol legacy Palm/HP webOS supports, then stand up a
self-hostable VPN server (on the user's Raspberry Pi) that the device's built-in VPN
app can connect to. Deliverables are a setup script + a community how-to guide the user
will host on GitHub for the webOS Archive community.

## Status: ✅ RELEASED — the OpenVPN agent is done, shipping, and validated on real hardware
The custom **OpenVPN VPN-agent** is complete: it connects an HP TouchPad through the
stock Settings → VPN UI to a real PiVPN server with TLS 1.3 / AES-256-GCM; traffic AND
DNS go through the tunnel and DNS is restored on disconnect. Packaged as a distributable
`.ipk` (info app "OpenVPN 2" + agent + postinst/prerm) and shipped to the community.
- Repo: **github.com/webOSArchive/webos-openvpn-2** (moved from codepoet80/webos-openvpn).
- **Read `RESUME.md` (repo root) first** — full status, exact ABI, build/install, next work.
- The older strongSwan/EasyVPN path is deprecated: `setup-webos-easyvpn-deprecated.sh`
  installs it, `uninstall-webos-easyvpn.sh` removes it (PiVPN-safe).

## Status: ✅ TAILSCALE AGENT WORKING — second agent, in `agent-tailscale/`
A **Tailscale (WireGuard) agent** now exists alongside the OpenVPN one, same ABI,
same "no app patch" design. Validated end-to-end through the stock Settings VPN
UI on a TouchPad: connect, tailnet IP, direct peer-to-peer NAT traversal, exit
node, MagicDNS, and clean disconnect. **Read `agent-tailscale/BUILD.md` first.**
- Backend is **tailscaled v1.78.1**, a ~23 MB **static Go** binary
  (`CGO_ENABLED=0 GOARCH=arm GOARM=7`). Go runs fine on the 2.6.35 kernel — the
  documented 3.2 floor is policy, not a hard break. Being static, it needs
  **no `ssl11`/OpenSSL package at all**, unlike the OpenVPN agent.
- Perf is a non-issue: ChaCha20-Poly1305 measured **146 Mbit/s on one core**;
  exit-node overhead ~17% (the device's Wi-Fi is the bottleneck). RSS ~44-76 MB.
- Architecture: the agent spawns ONE child, `scripts/tailscale-run`, which owns
  `tailscaled` + `tailscale up` and emits `TSAGENT-STATE:` markers the agent
  parses (same `g_io_add_watch` pattern as OpenVPN). Auth is a **pre-auth key**
  (interactive browser login can't work here); blank field falls back to
  `/media/internal/tailscale-authkey.txt` via `--auth-key=file:`.
- Package: `packaging-tailscale/build-ipk.sh` → ~9 MB `.ipk`. postinst/prerm both
  tested from a clean state. The 23 MB binary stays in the app dir on
  `/media/cryptofs` (root fs has ~120 MB free); `/usr/lib/vpn/agents/tailscale/
  tailscale` is a **symlink** to it (combined binary, argv[0] selects CLI mode) —
  cryptofs refuses symlinks, `/` is ext3, hence the direction.
- **Non-obvious traps (all cost real debugging, all documented in BUILD.md):**
  `rp_filter` must be 2/loose or exit-node mode blackholes itself; never parse
  tailscaled's `Switching ipn state` lines (its `NoState -> Stopped` at startup
  precedes `up`); checkbox `value` must be the STRING `"true"` (DynamicForm does
  `=== "true"`); answer a deferred disconnect on the runner's `disconnected`
  marker, not child exit (the daemon unloads the plugin first, so `on_run_exit`
  never fires); `tailscale up` needs `--timeout` or it blocks forever;
  `tailscaled --cleanup` does NOT reliably remove the ip rules.

### (historical) EasyVPN status
The strongSwan + vpnc EasyVPN tunnel was confirmed working earlier. Two follow-on threads
were active before the OpenVPN client superseded this path:
1. **Risk hardening of the existing path** (see "Encryption / risk profile" below).
   The user has **already IP-allowlisted** the Pi's 500/4500 to two known networks
   (their house + parents' house).
2. **New direction — write a custom webOS VPN *agent plugin* for a modern protocol
   (OpenVPN)** so the device can roam onto untrusted networks with strong crypto,
   *while keeping the stock Settings UI*. Architecture + ABI reverse-engineered this
   session (see "Custom agent plugin" below).

Note: this machine has **novacom access to the TouchPad only** — there is *no* Pi here
and we are *not* spoofing/emulating the Pi locally. The strongSwan server runs on the
user's actual Pi.

## Key findings (verified on-device)

Device: **HP TouchPad, webOS 3.0.5**, build `Nova-HP-Topaz` #86, kernel 2.6.35.
- novacom works as: `echo '<cmds>' | novacom run file://bin/sh` (we have **root**).
  Plain `novacom run 'cmd'` does NOT work — must be a `file://` URI to an executable.
- Device LAN IP (Wi-Fi = `eth0`): **192.168.5.224**, mask 255.255.252.0 → LAN is `192.168.4.0/22`.

webOS uses a **pluggable VPN "agent"** model in `/usr/lib/vpn/agents/`. Two agents ship:
| Agent | ID | Type | Backend | Protocol |
|---|---|---|---|---|
| **VPNC** | `com.palm.vpnc` | `IPSec` | **`vpnc 0.5.3`** (`/usr/sbin/vpnc`) | **Cisco EasyVPN**: IKEv1 **Aggressive Mode** + group PSK + **XAUTH** |
| Cisco AnyConnect | `com.palm.anyconnectagent` | `ssl` | `libVpnAcAgent.so` | AnyConnect SSL (TLS) |

- `pppd` exists but is **not** a VPN agent (it's cellular WAN PPP). **No PPTP/L2TP** UI option.
- **AnyConnect path is impractical**: device TLS = **OpenSSL 0.9.8k (2009)**, TLS 1.0 max,
  legacy ciphers only → modern ocserv won't negotiate. **IPsec/vpnc path chosen** because
  IKE/ESP crypto is independent of the rotten TLS stack.

### Confirmed vpnc invocation (from `libVpncAgent.so` strings)
- Runs `/usr/sbin/vpnc --non-inter --no-detach`, config piped to **stdin** (`vpnc.conf` format).
- Script: `/usr/lib/vpn/agents/vpnc/vpnc-script-palm`. PID in `/tmp/vpnc-pid`.
- Disconnect: `killall -s SIGHUP vpnc`.
- Form fields the agent collects → vpnc.conf: `IPSec gateway`, `IPSec ID`, `IPSec secret`,
  `Xauth username`, `Xauth password`, `Domain`, `NAT Traversal Mode` (natt / cisco-udp / force-natt).
- Default crypto: DH group 2 (modp1024), 3DES/AES-CBC, SHA1, aggressive mode.
- Profiles stored in `/var/palm/data/vpnframework.db`.

## Deliverables (in this folder)
- **`setup-webos-easyvpn-deprecated.sh`** — one-shot Pi installer (Debian/Raspbian). Installs strongSwan +
  `libcharon-extra-plugins` (xauth-generic!), enables forwarding + NAT + MSS clamp,
  writes config, enables aggressive-mode PSK, restarts, prints device field values.
  Run: `sudo GROUP_SECRET='...' VPN_USER='...' VPN_PASS='...' ./setup-webos-easyvpn-deprecated.sh`
  (syntax-checked; tunables are env-overridable: VPN_POOL, VPN_DNS, LEFT_SUBNET, WAN_IF).
- **`README.md`** — community guide (blog-style). Explains why IKEv2 guides fail,
  full manual steps, device field-mapping table, troubleshooting table, internals appendix.
- Source app `com.palm.app.vpn/` is the stock VPN front-end pulled off the device
  (Enyo 1; dynamic form driven by `getAgents` from VpnService — no hardcoded protocol).

### strongSwan config essentials (the non-obvious bits)
- `keyexchange=ikev1`, `aggressive=yes`, `authby=xauthpsk`, `rightauth2=xauth`, `xauth=server`.
- `/etc/strongswan.d/webos-easyvpn.conf`: `i_dont_care_about_security_and_use_aggressive_mode_psk = yes`
  and `cisco_unity = yes` — **required** or strongSwan won't answer vpnc at all.
- Legacy proposals: `ike=aes128-sha1-modp1024,aes256-sha1-modp1024,3des-sha1-modp1024`,
  `esp=aes128-sha1,aes256-sha1,3des-sha1`.
- `: PSK "group-secret"` (catch-all, any IPSec ID accepted) + `user : XAUTH "password"`.
- **`xauth-generic` plugin is the #1 gotcha** — without `libcharon-extra-plugins`, group key
  passes then XAUTH fails ("Wrong Username/Password" on device).

## Blog review verdict (user asked about vanwerkhoven.org IKEv2 guide)
That guide is **IKEv2 + EAP-MSCHAPv2 + certificates** → incompatible on the wire with
webOS's IKEv1 vpnc. Reusable: host setup (apt install, ip_forward, iptables MASQUERADE,
DDNS, port-forward). Replace: its entire connection config with our IKEv1 xauth-psk one.

## Encryption / risk profile of the shipped EasyVPN path
The working config is IKEv1 **Aggressive Mode + group PSK + XAUTH**. Known weaknesses:
- **Aggressive Mode PSK is remotely offline-crackable with only the public IP + open
  UDP/500 — no traffic sniffing needed.** An attacker *initiates* the handshake; the
  responder returns `HASH_R` (= `HMAC(HMAC(PSK, Ni|Nr), knowns)`) *before* the initiator
  authenticates. Every input but the PSK is then known → offline dictionary/brute-force
  (`ike-scan --aggressive -P` → `psk-crack`). Our **catch-all `: PSK`** makes it worse:
  the gateway hands `HASH_R` to anyone, no need to guess the group ID first.
- **fail2ban / rate-limiting is useless here** — one round-trip yields the hash; the crack
  is offline. Only PSK **entropy** defends it.
- Also weak: DH group 2 (modp1024, Logjam-tier), 3DES (Sweet32). AES-CBC+SHA1 is dated-OK.
- vpnc is **Aggressive-Mode-only** (`do_phase1_am`; no Main Mode code path) — you cannot
  reconfigure or lightly patch it into Main Mode. Main Mode wouldn't leak `HASH_R` to an
  anonymous initiator, but it's not reachable on this client.

**Mitigations (in priority order):**
1. **IP-allowlist 500/4500 to known source IPs** — done by user. This defeats the remote
   hash-grab (attacker can't receive `HASH_R`; UDP source-spoof doesn't return to them).
   *Cost:* breaks roaming — VPN only works *from* those networks. This is the main driver
   for the OpenVPN-agent effort below (roaming needs a client that isn't aggressive-mode).
2. **Long random group PSK** (`openssl rand -base64 24`, ~128+ bits) — makes the offline
   crack infeasible even if the hash leaks. Script default `ChangeThisGroupSecret` is
   crackable in minutes; treat the group secret as *public* the moment :500 is reachable.
3. Strong unique XAUTH password per user (the real access-control second factor).

## Custom agent plugin — write our own modern VPN client (KEEP the stock UI)
**Goal:** a new agent that drives **OpenVPN** (userspace, TUN), linked against the
modernized OpenSSL (codepoet80/OpenSSL-legacyWebOS = OpenSSL **1.1.1w**, TLS 1.2/1.3,
installed in `/usr/lib/ssl11`). Gives roaming + modern crypto + cert auth, no Aggressive
Mode, inside the built-in Settings VPN app.

**Feasibility verified this session — all green:**
- **Discovery is manifest-driven, not hardcoded.** VpnService scans `/usr/lib/vpn/agents/*/`
  and reads each `vpn-plugin-info.json`. Schema: `{title, id, version, vendor, type[], plugin}`
  (+ optional `icon`, `eula`). Drop in a third dir → it appears. **No service patch.**
- **The agent authors its own form fields** (proven: `libVpncAgent.so` builds the
  `vpnFormFields` JSON in code with `textfield`/`passwordfield`/`checkbox`/`listselector`
  and keys `vpnGroupId`/`vpnGroupSecret`/`vpnDomain`/`vpnNatTraversal`). The front-end
  (`com.palm.app.vpn`) is a pure renderer: `DynamicForm.js` draws whatever the agent
  supplies; `VpnApp.js handleLaunch` receives `vpnFormFields` via `enyo.windowParams`.
  So we declare OpenVPN fields (server/port/proto/CA/cert/key/user/pass) → native UI free.
  **No app patch.** The `type` string (`IPSec`/`ssl`) is cosmetic label only; reuse `ssl`.
- **`/dev/net/tun` exists** (char 10,200) → OpenVPN data path works.

**Agent ABI (reverse-engineered from `reversing/libVpncAgent.so`, unstripped ARM ELF,
plain C — NOT C++, no vtables). Local copy + `reversing/disasm.txt` (llvm-objdump) saved.**
- Exported entry points VpnService dlopens: **`initVpnAgent(ctx, …)`** and **`cleanupVpnAgent()`**.
- `initVpnAgent`: allocates ~224 B private state, then fills the **caller-owned descriptor
  struct `ctx`**: flags/version word at +0x00, agent-id string (`strncpy`, 32-B field) at
  +0x04, and **5 op function pointers at +0x28/+0x2c/+0x30/+0x34/+0x38** = the ops table
  (connect / disconnect / getConnectionDetails / getInterfaceInfo / notifySystemChange —
  matching internal `vpnc_connect` / `vpnc_disconnect` / `vpnc_get_connection_details` /
  `vpnc_get_interface_info` / `vpnc_notify_system_change`). Then `loadLocalizedStrings()`.
- Request/response protocol = **json-c** (`libcjson.so`) via a `cmd_request` object: params
  in `cmd_request_get_json`, reply `cmd_request_send_response`, id `cmd_request_get_request_id`.
  Field values read by key via `getValueFromProfileDetails`. State pushed via
  `notify_connection_state` (`connecting`/`connected`/`disconnecting`/`disconnected`/`reconnecting`).
- Deps (all present on device): `libcjson.so`, `libglib-2.0.so.0`, `libpthread`, `libgcc_s`, `libc`.
- Backend pattern to clone: `write_config_to_vpnc_stdin` → spawn `/usr/sbin/vpnc
  --non-inter --no-detach` → `handleVpncReadOutput` parses stdout for state → routes via
  `vpnc-script-palm` → pid `/tmp/vpnc-pid`. Our version: spawn `openvpn`, parse its output,
  route via an `openvpn-up` script (clone `vpnc-script-palm`'s env-var contract).

**Remaining before writing the skeleton:** confirm exact `ctx` struct offsets + which op
emits the field schema (disassemble the op that builds the listselector JSON). Then the two
build pieces: (A) `/usr/lib/vpn/agents/openvpn/` = `vpn-plugin-info.json`
(`id: org.webosarchive.openvpn`, `plugin: libVpnOpenvpnAgent.so`) + small C `.so` cloning
the vpnc agent + `openvpn-up` script + `resources/*/strings.json`; (B) **cross-compile
`openvpn` for ARM EABI5 / old glibc / kernel 2.6.35, linked against OpenSSL 1.1.1w** — the
biggest chunk, same toolchain class as the OpenSSL project. Reversing risk now LOW; main
work is the backend cross-compile + up-script + testing.

### ABI fully recovered this session (exact)
Descriptor struct `vpnd` passes to `initVpnAgent(ctx, h0,h1,h2)` — offsets read from the
binary + `.got` RELATIVE relocs: `+0x00` version(=1), `+0x04` id[32], then op ptrs
`+0x28` connect (`vpnc_connect`), `+0x2c` disconnect, `+0x30` getConnectionDetails,
`+0x34` handleClientUiPromptResponse, `+0x38` notifySystemChange; `sizeof=0x3c` (=memset 60).
Field schema is built by `build_ui_credentials_json` (json-c) → shipped via
`promptUserForCredentials`; UI contract is `DynamicForm.js` (`id`/`type`/`label`/`value`/
`options[]`). `vpnHost` is collected separately by the Add-Profile scene. Deps: libcjson
(Palm's json-c, link `-lcjson`), libglib-2.0, libpthread. Still fuzzy: op arg signatures +
the `h0..h2` host-callback vtable (only needed for M2 request/response, not M1 registration).

## ✅ OPENVPN AGENT WORKING END-TO-END (validated on the TouchPad this session)
### → Read `RESUME.md` first (repo root): full status, ABI, build/install, test steps, next work.
The custom OpenVPN agent is **built, installed, and confirmed working on-device**.
Full ABI recovered from both `libVpncAgent.so` and `PmVpnDaemon` and validated by
behaviour. What works: registers in stock UI (`getAgents` lists OpenVPN), prompts
for OpenVPN settings via the stock configure-profile scene, saves profiles,
Connect → spawns cross-compiled **OpenVPN 2.5.9 vs OpenSSL 1.1.1w** (runs on
device), state flows to the UI, Disconnect kills it cleanly — no daemon crashes
across repeated cycles.

**Build/toolchain:** Linaro 4.9.4 `arm-linux-gnueabi` at
`/opt/gcc-linaro-4.9.4-2017.01-...`; agent links device `libcjson.so` +
`libglib-2.0.so.0` (sysroot in `agent-openvpn/sysroot/`, headers shimmed:
`json.h`, `glib_shim.h`). OpenVPN built in `build-openvpn/openvpn-2.5.9` against
`/home/jonwise/Projects/OpenSSL-legacyWebOS/openssl-1.1.1w`. Install:
`agent-openvpn/install-openvpn-agent.sh` (novacom). Device log:
`/var/log/webos-openvpn-agent.log`.

**Key ABI facts (all confirmed, see `agent-openvpn/src/webos_vpn_agent_abi.h`):**
- `initVpnAgent` **returns int 0** (nonzero ⇒ daemon dlcloses the plugin).
- **op signatures are NOT uniform** (each verified at its daemon call site):
  `connect`(0x28) & `handle_ui_prompt_resp`(0x34) = `(token, params_json, cb)` — cb=arg2;
  `disconnect`(0x2c) & `get_connection_details`(0x30) = `(token, cb)` — **cb=arg1, no
  params**; `notify_system_change`(0x38) = `(token)`, no reply. Reading cb from the
  wrong arg calls garbage and **SIGSEGVs the daemon** (this is what crashed disconnect —
  the tunnel dropped but the app got "Message status unknown"). For the params ops,
  arg1 is a JSON **string** — `json_tokener_parse` it; treating it as an object segfaults.
- reply `cb(token, 0, code, errText)`: 0=success; **-7 = silent in ALL app scenes**
  (use after launching the profile/creds form; -6 is silent only in the modal prompt).
- host table @ daemon `.data 0x21988`: host0=`SendMsgToApp(char* json)` (prompt),
  host1=`Notify(char* json)` (status), host2=`addNetworkInterface`, … host11=
  `profileGetPrvDetails`. host0/host1 are **main-thread-only** ⇒ agent uses GLib
  `g_io_add_watch`/`g_child_watch_add` on the daemon loop (no background thread).
- prompt envelope to host0: `{vpnAgentGuid, vpnMsgType:"credentials", vpnHost?,
  vpnProfileName?, vpnFormFields:[{id,type,label,value,editable,options?}]}`.
- status envelope to host1: `{vpnProfileName, state}` where state ∈
  connecting/connected/disconnecting/disconnected/reconnecting.
- Connect gate: agent must have EULA accepted (`acceptEula`) and, for Disconnect
  to dispatch, the profile must be saved (`addProfile`) so the daemon tracks it
  active — both are automatic in the real UI flow.

**Done since:** DNS hand-off (openvpn-up repoints webOS dnsmasq at the pushed VPN DNS,
restores on disconnect — traffic + DNS both tunneled, leak-free); `.ipk` packaging;
per-op ABI + disconnect-crash fix. **Remaining polish:** deeper `addNetworkInterface`
DNS/route integration (current resolv.conf-rewrite works); roaming. Pi side + full
walkthrough in repo-root **`PIVPN-GUIDE.md`**.

## (historical) M1 SCAFFOLD — in `agent-openvpn/` (see `agent-openvpn/BUILD.md`)
Files: `vpn-plugin-info.json` (id `org.webosarchive.openvpn`, type `ssl`, plugin
`libVpnOpenvpnAgent.so`), `src/webos_vpn_agent_abi.h` (ABI + compile-time offset asserts,
guarded to 32-bit), `src/openvpn_agent.c` (fills descriptor + 5 stub ops that log to
`/var/log/webos-openvpn-agent.log` + json-c OpenVPN field builder), `scripts/openvpn-up`,
`Makefile` (cross-compile → `.so`; set `CROSS_COMPILE`/`SYSROOT`), and
`scripts/get-palm-libcjson.sh` (run on a host with the device on novacom: pulls
`/usr/lib/libcjson.so*` — Palm's json-c — into `$SYSROOT/usr/lib`; production
images strip `/usr/include`, so it prints how to backfill json-c 0.9.x headers).
Host-side clang shows
`json.h not found` + json_object errors — EXPECTED off-target (headers live in the cross
sysroot); ABI asserts inert on 64-bit host. Everything documented for cloning to the Linux
box with the GCC toolchain.

## Build/deploy/test workflow (the plugin is built & released; this is how to iterate)
- **Build agent:** `cd agent-openvpn && make` (Linaro `arm-linux-gnueabi`; in-tree
  `sysroot/` has device libcjson/libglib + shim headers `json.h`/`glib_shim.h`).
- **Deploy for dev:** `novacom put file:///usr/lib/vpn/agents/openvpn/libVpnOpenvpnAgent.so
  < libVpnOpenvpnAgent.so`; then `killall PmVpnDaemon` so it re-dlopens the new `.so`.
- **Build the `.ipk`:** `cd packaging && ./build-ipk.sh` → `packaging/dist/` (syncs the
  agent payload from `agent-openvpn/`, injects postinst/prerm). Install via **Preware /
  WebOS Quick Install** (NOT palm-install — postinst needs root).
- **Dev gotchas:** after `killall PmVpnDaemon`, the daemon persists connection state, so
  the first `connect` to a profile it still thinks is connected NO-OPS — `disconnect`
  first. `PmVpnDaemon` is on-demand: a dead `ps` grep is a normal idle-exit, not a crash
  (check `/var/log/messages` for `SIGSEGV`/`minicore`, and crash minicores under
  `/var/log/reports/librdx/*PmVpnDaemon*minicore*.gz` for a backtrace).
- **Info app:** a bare HTML webOS app must call `PalmSystem.stageReady()` on load or the
  card pulses forever on the splash (frameworks do this; a plain page must too).

## webos-mcp knowledge
The `webos-mcp` server exposes `webos://knowledge/<topic>` resources (and a full bundle at
`webos://knowledge/all`, ~170k tokens). Loaded last session: overview, services, js-services,
ls2-roles, system-internals, tls-and-networking, postinst-packaging, nizovn-packages,
sdk-tools, gotchas, app-structure, pdk. Re-load specific topics with ReadMcpResourceTool
as needed; the `all` bundle is too big to Read directly (chunk via individual resources).
