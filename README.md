# OpenVPN for legacy webOS

**A modern, roaming-safe VPN on a 2011 tablet — through the VPN screen that's already built into webOS.**

This project adds **OpenVPN** support to the VPN manager built into **Settings → VPN**
on Palm/HP webOS. It's a native VPN *agent plugin* (`org.webosarchive.openvpn`) that
drives a cross-compiled **OpenVPN 2.5.9** linked against **OpenSSL 1.1.1w**, so an
HP TouchPad (webOS 3.0.5) — or the webOS 2.x phones with the same VPN client — can
connect to any standard OpenVPN server with **TLS 1.2/1.3 and AES-256-GCM**.

No jailbreak-y app that reimplements a VPN UI: you set up and manage connections in
the **stock Settings → VPN app**, exactly like the built-in Cisco options. The plugin
just teaches the OS a new protocol.

> Verified working end-to-end on a real HP TouchPad against a Raspberry Pi running
> [PiVPN](https://pivpn.io): full-tunnel routing **and DNS** go through the tunnel,
> and your normal DNS is restored on disconnect.

> **Also here: a [Tailscale agent](TAILSCALE-GUIDE.md).** Same plugin model, same
> stock UI, but no server to run, no port-forward, and no extra packages on the
> device — plus NAT traversal and exit nodes.

---

## Why

webOS ships a capable VPN client in Settings, but out of the box it only speaks
**Cisco EasyVPN / IKEv1** (and an AnyConnect SSL mode that can't negotiate with modern
servers). That legacy path works, but its crypto is weak and it can't safely roam onto
untrusted Wi-Fi (see [the older EasyVPN guide](#legacy-alternative-ikev1-easyvpn) below).

The device's *system* OpenSSL is the rotten **0.9.8** stack from 2009 (TLS 1.0 max), so
you can't just point it at a modern VPN. The trick: the agent's bundled `openvpn` links
a **side-by-side modern OpenSSL** (`/usr/lib/ssl11`, from
[codepoet80/OpenSSL-legacyWebOS](https://github.com/codepoet80/OpenSSL-legacyWebOS)),
so the tunnel's crypto runs entirely in TLS 1.3-capable code, independent of the ancient
system libraries.

| | Built-in "VPNC" (EasyVPN) | **This OpenVPN agent** |
|---|---|---|
| Key exchange | IKEv1 **Aggressive Mode** | **TLS 1.2 / 1.3** |
| PSK exposure | `HASH_R` leaks to any prober → offline-crackable | `tls-crypt` — server won't answer without the key |
| Data cipher | 3DES / AES-CBC + SHA1 | **AES-256-GCM** |
| Safe to roam on untrusted Wi-Fi? | No | **Yes** |
| Crypto library | system OpenSSL 0.9.8 (2009) | bundled **OpenSSL 1.1.1w** |
| UI | Stock Settings → VPN | Stock Settings → VPN |

---

## Requirements

- An **HP TouchPad** (webOS 3.0.5) — or a webOS 2.x device with the built-in VPN app —
  with developer/root access (novacom), which is standard in the community.
- The **[OpenSSL-legacyWebOS](https://github.com/codepoet80/OpenSSL-legacyWebOS)** package
  installed (the "TLS 1.3 Updates for TouchPad" in the Preware *modernize* feed). It
  provides `/usr/lib/ssl11/libssl.so.1.1` + `libcrypto.so.1.1` that the bundled `openvpn`
  links at runtime.
- `/dev/net/tun` (present on stock webOS 3.0.5).
- An **OpenVPN server** you control and its `.ovpn` client profile. A Raspberry Pi with
  PiVPN is the reference setup — see the guide below.

---

## Quick start

1. **Server:** set up OpenVPN on your Pi (`curl -L https://install.pivpn.io | bash`,
   choose *OpenVPN*), then `pivpn add` to generate a client `.ovpn`.
2. **Install the plugin:** grab the latest **`org.webosarchive.openvpn_*.ipk`** and install
   it with **Preware** or **WebOS Quick Install**. (Not `palm-install` — it can't run the
   root install step. See [why](#packaging).)
3. **Copy your `.ovpn`** onto the device (USB drive mode → `/media/internal/`).
4. **In Settings → VPN → Add Profile:** choose **OpenVPN**, enter the server address, set
   the **Config file** path to your `.ovpn`, add the **key passphrase** if it has one, and
   **Save & Connect**.

**→ Full step-by-step (Pi + device), with troubleshooting: [`PIVPN-GUIDE.md`](PIVPN-GUIDE.md).**

---

## How it works

webOS's VPN daemon (`PmVpnDaemon`) discovers agents by scanning
`/usr/lib/vpn/agents/*/vpn-plugin-info.json` and `dlopen`-ing each plugin, and the
Settings app renders whatever form fields the agent declares. So a third agent slots in
with **no patching of the daemon or the app**:

```
Settings → VPN app  (stock, unmodified)
        │  getAgents / connect / disconnect / addProfile …
        ▼
PmVpnDaemon  (stock, unmodified)
        │  dlopen + initVpnAgent + op dispatch
        ▼
libVpnOpenvpnAgent.so   ← this project
        │  generate config, spawn + monitor openvpn, report state,
        │  hand DNS to the tunnel (openvpn-up) and restore on disconnect
        ▼
openvpn 2.5.9  →  OpenSSL 1.1.1w (/usr/lib/ssl11)  →  tun0  →  your server
```

The agent ABI was reverse-engineered from the stock `libVpncAgent.so` **and**
`PmVpnDaemon` (both unstripped) and confirmed on-device. If you want the gory details —
the descriptor layout, the non-uniform op signatures, the host callback table, the DNS
hand-off — it's all in [`agent-openvpn/BUILD.md`](agent-openvpn/BUILD.md).

---

## Repository layout

| Path | What |
|---|---|
| [`agent-openvpn/`](agent-openvpn/) | The agent plugin: C source, the reverse-engineered ABI header, Makefile (Linaro cross-compile), the `openvpn-up` route/DNS script, and a novacom install script. See [`BUILD.md`](agent-openvpn/BUILD.md). |
| [`packaging-openvpn/`](packaging-openvpn/) | Builds the distributable `.ipk` (`build-ipk.sh`) — a small info app that bundles the agent + `postinst`/`prerm` that install it as root. |
| [`PIVPN-GUIDE.md`](PIVPN-GUIDE.md) | End-to-end guide: PiVPN server + TouchPad client + troubleshooting. |
| [`agent-openvpn/BUILD.md`](agent-openvpn/BUILD.md) | Build + architecture + the full recovered agent ABI. |
| [`agent-tailscale/`](agent-tailscale/) | A **second agent**, same plugin model, driving Tailscale (WireGuard). See [`TAILSCALE-GUIDE.md`](TAILSCALE-GUIDE.md). |
| [`packaging-tailscale/`](packaging-tailscale/) | Builds the Tailscale `.ipk`, same shape as `packaging-openvpn/`. |
| [`ipks/`](ipks/) | The latest built `.ipk` for each agent, kept in git so you can install without building. |
| `com.palm.app.vpn/` | The stock webOS VPN front-end (pulled off the device) — the rendering contract the agent targets. |
| `reversing/` | The stock `libVpncAgent.so` / `PmVpnDaemon` + disassembly used to recover the ABI. |
| `setup-webos-easyvpn-deprecated.sh` | The **older** IKEv1/EasyVPN server installer (see below). |
| `uninstall-webos-easyvpn.sh` | Fully removes that EasyVPN/strongSwan server (safe alongside PiVPN). |

### Build it yourself

```sh
# 1. cross-compile the agent (needs the Linaro arm-linux-gnueabi toolchain + a sysroot
#    with the device's libcjson/libglib — see agent-openvpn/BUILD.md)
cd agent-openvpn && make

# 2. build the .ipk (needs the HP webOS SDK for palm-package)
cd ../packaging-openvpn && ./build-ipk.sh   # → ipks/*.ipk
```

Building `openvpn` itself for the device is a one-time step documented in `BUILD.md §8`
(OpenVPN 2.5.9, Linaro toolchain, linked against the OpenSSL-legacyWebOS 1.1.1w tree).

<a name="packaging"></a>
> **Install via Preware / WebOS Quick Install, not `palm-install`.** The `.ipk`'s
> `postinst` copies the agent into `/usr/lib/vpn/agents/openvpn/` and restarts the VPN
> daemon — that runs as root only under Preware/WQI. `palm-install` runs unprivileged, so
> the plugin never gets installed.

---

## Status & limitations

**Working:** register in the UI, configure a profile, connect (cert / cert+passphrase),
full-tunnel routing, DNS through the tunnel with clean restore on disconnect, and
disconnect — all validated on-device against a live PiVPN, with no daemon crashes.

**Rough edges / TODO:**

- **DNS during a mid-session network change.** DNS is repointed at the tunnel via webOS's
  local dnsmasq and restored on disconnect; if the OS rewrites its resolver during a Wi-Fi
  change mid-session, DNS can briefly revert until the next connect. A deeper
  connection-manager integration (`addNetworkInterface`) would make it bulletproof.
- **Cert import UX.** Today you point the agent at an `.ovpn` file path on the device
  (robust and simple). A nicer on-device import flow is possible.
- **Roaming/reconnect** relies on openvpn's own `--up-restart`.

<a name="legacy-alternative-ikev1-easyvpn"></a>
### Legacy alternative: IKEv1 / EasyVPN

Before this plugin existed, the only way to use the built-in client was its native
**Cisco EasyVPN (IKEv1 aggressive mode)** path. `setup-webos-easyvpn-deprecated.sh` still stands up a
strongSwan server for it, and it works — but it uses deliberately weak legacy crypto
(aggressive-mode group-PSK, modp1024/3DES/SHA1) that is **not safe to roam** and whose
group secret is remotely crackable from just your public IP. **Prefer the OpenVPN agent.**
The old path is kept for completeness and for devices where the OpenSSL-legacyWebOS
package isn't an option.

Already ran the old installer and want it gone? `sudo ./uninstall-webos-easyvpn.sh`
purges strongSwan and undoes its firewall/sysctl changes — and leaves a PiVPN server
on the same box untouched. (Then remove the UDP 500/4500 port-forward on your router.)

---

## Credits

- Built for the **webOS Archive** community — <https://docs.webosarchive.org>.
- Modern crypto courtesy of [codepoet80/OpenSSL-legacyWebOS](https://github.com/codepoet80/OpenSSL-legacyWebOS)
  (OpenSSL 1.1.1w for webOS).
- Bundles [OpenVPN](https://openvpn.net/) 2.5.9 and links OpenSSL 1.1.1w — each under its
  own license.

HP open-sourced/abandoned webOS over a decade ago; the reversing artifacts here are the
ground truth for an ABI that was never publicly documented. Contributions and bug reports
welcome.
