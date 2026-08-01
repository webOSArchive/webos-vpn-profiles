# OpenVPN for legacy webOS — PiVPN server + TouchPad client

Run a **modern, roaming-safe OpenVPN tunnel** on an HP TouchPad (webOS 3.0.5),
using the stock **Settings → VPN** UI. The client is a native VPN *agent plugin*
(`org.webosarchive.openvpn`) that drives a cross-compiled **OpenVPN 2.5.9** linked
against **OpenSSL 1.1.1w** — so you get TLS 1.2/1.3, AES-GCM, and `tls-crypt`,
none of which the device's rotten 2009-era stack can do on its own.

The server side is a Raspberry Pi running **[PiVPN](https://pivpn.io)** in OpenVPN
mode. PiVPN's modern defaults work as-is — you do **not** have to weaken anything
for the old client, because the crypto runs in the bundled OpenSSL 1.1.1w, not the
device's system OpenSSL 0.9.8.

> **Why not the built-in Cisco IPsec path?** The stock "VPNC" agent only speaks
> IKEv1 **Aggressive Mode + group PSK**, which is offline-crackable from just your
> public IP (see `README.md`). That path works but can't safely roam onto untrusted
> Wi-Fi. This OpenVPN agent is the roaming-safe replacement.

---

## What you need

- A **Raspberry Pi** (any model) reachable from the internet — a port-forward or
  DDNS on your router, exactly like any self-hosted VPN.
- An **HP TouchPad** on webOS 3.0.5 with:
  - **novacom** access from a Linux/Mac box (root shell — standard for the dev
    community).
  - the **OpenSSL-legacyWebOS** package installed (provides
    `/usr/lib/ssl11/libssl.so.1.1` + `libcrypto.so.1.1`). The agent's openvpn
    binary links these at runtime via `LD_LIBRARY_PATH=/usr/lib/ssl11`.
- The build artifacts from `agent-openvpn/` (prebuilt, or `make` them yourself —
  see `agent-openvpn/BUILD.md`).

---

## Part 1 — Set up the Pi with PiVPN (OpenVPN mode)

### 1. Install PiVPN

On the Pi (Raspberry Pi OS / Debian):

```sh
curl -L https://install.pivpn.io | bash
```

Walk through the installer and choose:

| Prompt | Choose | Why |
|---|---|---|
| VPN type | **OpenVPN** | (not WireGuard — the device has no WireGuard client) |
| Encryption | **256-bit** (or 224-bit EC) | OpenSSL 1.1.1w on the device handles it fine |
| Certificate type | **ECDSA** (default) or RSA-2048 | either works; ECDSA is smaller/faster |
| Protocol | **UDP** on **1194** (default) | TCP/443 also fine if UDP is blocked on hostile Wi-Fi |
| DNS | your choice (e.g. the Pi via `unbound`, or a public resolver) | see the DNS caveat below |
| Public IP vs DNS | your **DDNS hostname** if your IP changes | this becomes the `.ovpn`'s `remote` |

PiVPN's defaults produce a **`tls-crypt`-wrapped, AES-256-GCM, TLS 1.2+** setup.
That is exactly what we want — modern and roaming-safe.

### 2. Open the port on your router

Forward **UDP 1194** (or whatever you chose) from the internet to the Pi, and make
sure your DDNS hostname resolves to your public IP. This is ordinary VPN hosting;
PiVPN prints reminders at the end.

> Unlike the old IPsec/EasyVPN path, you do **not** need to IP-allowlist the port.
> OpenVPN with `tls-crypt` won't even answer — let alone leak a crackable hash — to
> anyone who doesn't hold the `tls-crypt` key, so it's safe to expose and to roam.

### 3. Create a client profile

```sh
pivpn add            # prompts for a name, e.g. "touchpad"
# or non-interactively:
pivpn add -n touchpad
```

This writes `~/ovpns/touchpad.ovpn` — a **single self-contained file** with the
server address, port, protocol, cipher, and the inline `ca` / `cert` / `key` /
`tls-crypt` material. That one file is all the client needs.

Optional sanity check of what got baked in:

```sh
grep -E '^(remote|proto|cipher|data-ciphers|auth|tls-)' ~/ovpns/touchpad.ovpn
```

The device's OpenVPN 2.5.9 understands all of these. If your PiVPN is old enough to
emit only the deprecated `cipher AES-256-CBC`, that still works; newer PiVPN emits
`data-ciphers AES-256-GCM` which is preferred.

---

## Part 2 — Get the `.ovpn` onto the TouchPad

Copy `touchpad.ovpn` to the device. Any of:

- **USB drive mode**: plug the TouchPad into a computer, copy the file into the USB
  volume (it appears under `/media/internal/`), then create a folder for it.
- **scp/novacom** from your Linux box:
  ```sh
  # from the box with novacom + the file:
  echo 'mkdir -p /media/internal/vpn' | novacom run file://bin/sh
  novacom put file:///media/internal/vpn/client.ovpn < touchpad.ovpn
  ```
- **Download** it in the device browser and move it.

Remember the on-device path — e.g. **`/media/internal/vpn/client.ovpn`**. You'll
type it into the profile.

---

## Part 3 — Install the OpenVPN agent on the TouchPad

### Option A (recommended): the `.ipk`

Install **`org.webosarchive.openvpn_1.0.0_all.ipk`** with **Preware** or **WebOS
Quick Install**. It drops an "OpenVPN" info app in your launcher and, via its
`postinst`, installs the agent into `/usr/lib/vpn/agents/openvpn/` and restarts
the VPN daemon.

> **Must use Preware or WebOS Quick Install** — they run the install script as
> root. **`palm-install` will NOT work** (it runs unprivileged, so the agent never
> gets copied into place). This is the #1 support gotcha.

Prereq: the **OpenSSL-legacyWebOS** package (`/usr/lib/ssl11`) must be installed
first — the bundled `openvpn` links its `libssl.so.1.1` / `libcrypto.so.1.1`.

(Building the `.ipk` yourself: `cd packaging-openvpn && ./build-ipk.sh` → `ipks/`.)

### Option B: novacom install script (for development)

From a box with novacom + the build artifacts:

```sh
cd agent-openvpn
./install-openvpn-agent.sh
```

It checks prerequisites, pushes the files into `/usr/lib/vpn/agents/openvpn/`, and
restarts the daemon. See `agent-openvpn/BUILD.md` to rebuild from source.

To confirm registration:

```sh
echo "luna-send -n 1 palm://com.palm.vpn/getAgents '{}'" | novacom run file://bin/sh
```

You should see `"vpnAgentGuid": "org.webosarchive.openvpn", "vpnAgentLabel": "OpenVPN"`.

---

## Part 4 — Create the profile on the device

On the TouchPad:

1. **Settings → VPN → Add Profile**.
2. **Connection Type → OpenVPN**.
3. **VPN Server**: this **overrides** the `remote` baked into your `.ovpn`, so
   enter whichever endpoint you actually want to reach:
   - **Testing on your home LAN?** Enter the Pi's **local IP** (e.g.
     `192.168.10.2`). PiVPN usually bakes your *public* IP into the `.ovpn`, and
     that won't reach the Pi from inside your own network unless your router does
     NAT hairpin (many don't) — you'd get `TLS key negotiation failed`.
   - **Connecting from outside?** Enter your **DDNS hostname / public IP** and
     make sure the port is forwarded.
   Same `.ovpn` works both ways — just change this field. Tap **Next**.
4. The OpenVPN form appears. Fill it in:
   | Field | Value |
   |---|---|
   | **Server Port** | `1194` (or your port) |
   | **Protocol** | `UDP` (or `TCP`) |
   | **Authentication** | **Certificate (.ovpn)** — PiVPN default. Choose **Certificate + Login** only if you added a server-side auth plugin. |
   | **Config file (.ovpn) path** | `/media/internal/client.ovpn` (where you copied it) |
   | **Username (login only)** | leave blank for cert auth |
   | **Password / key passphrase** | **the passphrase you set when running `pivpn add`** — PiVPN encrypts the client's private key with it, and the client must supply it to decrypt the key. Leave blank only if you created the client with `pivpn add nopass`. |
5. **Save**, then **Connect**.

> **About that password:** `pivpn add` asks "Enter a pass phrase for the client" —
> that phrase *encrypts the private key inside the `.ovpn`*. It is **not** a
> login. Put it in the **Password / key passphrase** field with Authentication =
> **Certificate (.ovpn)**. If you leave it blank, openvpn will sit waiting for the
> key password and the connect spinner will hang forever. If you'd rather not deal
> with it, regenerate the client unencrypted: `pivpn add nopass -n touchpad`.

The agent generates an OpenVPN config that `config`-includes your `.ovpn` (so the
`.ovpn`'s own `remote`, cipher and inline certs are authoritative — one profile
survives a DDNS change), spawns `openvpn` against OpenSSL 1.1.1w, and reports the
connection state back to the VPN UI.

---

## Verifying & troubleshooting

**Watch the agent + openvpn output live** (over novacom):

```sh
echo 'tail -n 200 -f /var/log/webos-openvpn-agent.log' | novacom run file://bin/sh
```

Success looks like openvpn logging **`Initialization Sequence Completed`**, at which
point the UI flips to **Connected**.

| Symptom (in the log) | Cause / fix |
|---|---|
| **Connect spinner hangs forever** (no error) | Encrypted `.ovpn` key + no passphrase given → openvpn is silently waiting for the key password. Enter your `pivpn add` passphrase in **Password / key passphrase**, or use `pivpn add nopass`. |
| `EVP_DecryptFinal_ex:bad decrypt` / `private key password verification failed` | Wrong key passphrase. Re-enter the exact phrase you set in `pivpn add`. |
| `Options error: You must define CA file (--ca)` | The `.ovpn` path is wrong/empty, so the agent fell back to a cert-less config. Fix the **Config file** path; confirm the file exists on the device. |
| `Cannot resolve host address` | DDNS/host wrong, or no internet. openvpn retries. |
| `TLS Error: TLS handshake failed` / `TLS key negotiation failed to occur within 60 seconds` | Can't reach the server on the wire. On-LAN: you probably used the public IP — put the Pi's **local** IP in VPN Server. Remote: port not forwarded, wrong port/proto, or server down (`sudo systemctl status openvpn` on the Pi). |
| **Connects (tun0 up, can ping `10.x.x.1`) but no internet** | The tunnel is fine; the **Pi isn't NAT-forwarding** to the internet. On the Pi: `pivpn -d` (auto-fixes most), or check `sudo sysctl net.ipv4.ip_forward` (=1) and the MASQUERADE rule's outbound interface (`sudo iptables -t nat -L POSTROUTING -n -v`). |
| `Options error: ...block-outside-dns` | Harmless — a Windows-only pushed option openvpn ignores on Linux. The tunnel still comes up. |
| `AUTH_FAILED` | Using **Certificate + Login** without a server-side auth plugin, or bad credentials. Switch to **Certificate (.ovpn)**. |
| `Cannot open TUN/TAP dev /dev/net/tun` | `/dev/net/tun` missing — reboot the device; it's normally present. |
| openvpn exits immediately, `error while loading shared libraries: libssl.so.1.1` | OpenSSL-legacyWebOS not installed. Install it so `/usr/lib/ssl11` exists. |

You can also run the binary by hand to isolate problems:

```sh
echo 'LD_LIBRARY_PATH=/usr/lib/ssl11 /usr/lib/vpn/agents/openvpn/openvpn \
  --config /media/internal/vpn/client.ovpn --verb 4' | novacom run file://bin/sh
```

---

## Known limitations (v0.1)

These are honest gaps, not showstoppers — the tunnel comes up and routes IP:

- **DNS goes through the tunnel** (as of the latest build). `openvpn-up` repoints
  webOS's local dnsmasq upstream at the VPN-pushed DNS on connect and restores your
  normal resolver on disconnect, so lookups are tunneled and don't leak. One rough
  edge: if the OS rewrites its DNS upstream during a mid-session network change, DNS
  can briefly revert until the next connect; a deeper connection-manager integration
  (`addNetworkInterface`) would make it bulletproof.
- **Full-tunnel vs split-tunnel** depends on what your `.ovpn` pushes
  (`redirect-gateway`). PiVPN defaults to full-tunnel.
- **Reconnect on network change** is basic (openvpn's own `--up-restart` / ping
  restart). Deep roaming handoff is future work.

---

## Why this is the safe option

| | Built-in "VPNC" (EasyVPN) | This OpenVPN agent |
|---|---|---|
| Key exchange | IKEv1 **Aggressive Mode** | TLS 1.2 / 1.3 |
| Pre-shared secret exposure | `HASH_R` leaks to any anonymous prober → **offline-crackable** | `tls-crypt` — server won't respond without the key |
| Data cipher | 3DES / AES-CBC + SHA1 | **AES-256-GCM** |
| Safe to roam on untrusted Wi-Fi? | No (needs IP-allowlisting, which breaks roaming) | **Yes** |
| Crypto library | device OpenSSL 0.9.8 (2009) | bundled **OpenSSL 1.1.1w** |

Same stock Settings UI, modern crypto underneath.
