# Tailscale for legacy webOS — a mesh VPN on a 2011 TouchPad

This adds **Tailscale** to the VPN manager already built into webOS
(**Settings → VPN**). No patched apps, no replacement UI — the stock VPN
front-end grows a "Tailscale" connection type, and it behaves like any other
profile: tap to connect, tap to disconnect.

Underneath it is real Tailscale: WireGuard, NAT traversal (your TouchPad talks
*directly* to peers, not through a relay, when it can), exit nodes, subnet
routes and MagicDNS.

**Why you might prefer this to the OpenVPN agent:**

| | OpenVPN agent | Tailscale agent |
|---|---|---|
| Server to run | yes — a Pi with PiVPN | **none** |
| Port-forward on your router | yes | **no** |
| Extra packages on device | needs the TLS 1.3 / OpenSSL 1.1.1w package | **none** |
| Reaching other machines | whatever the Pi routes | every device on your tailnet |
| Roaming onto hotel Wi-Fi | works | works, and NAT traversal usually still finds a direct path |

The catch: you sign in with a **pre-generated auth key** rather than the usual
browser login (see *Why a key?* below).

---

## What you need

- An **HP TouchPad** on webOS 3.0.5, with **Preware** (or WebOS Quick Install)
  installed. Root/homebrew access is what Preware already gives you.
- A **Tailscale account** (the free plan is plenty) — or your own
  [Headscale](https://github.com/juanfont/headscale) server.
- About **25 MB** free on the media partition.

Not needed: a server, a static IP, a port-forward, dynamic DNS, or the
OpenSSL/TLS-1.3 package that the OpenVPN agent depends on. Tailscale ships as a
single static binary that carries its own modern TLS, so the device's ancient
system OpenSSL is simply not involved.

---

## Part 1 — Create an auth key

1. On a phone or computer, open the [Tailscale admin
   console](https://login.tailscale.com/admin/settings/keys) →
   **Settings → Keys**.
2. **Generate auth key**.
   - Leave **Ephemeral** *off* — an ephemeral node disappears when it goes
     offline, which is not what you want for a tablet.
   - **Reusable** is worth ticking. If you ever reinstall the app you'll need
     to authenticate again, and a reusable key saves a trip back here.
3. Copy the `tskey-auth-…` string.

### Why a key, and not the normal sign-in?

Signing in to Tailscale normally means approving it on a web consent screen.
The TouchPad's browser engine is from 2009 and cannot render a modern one — it
goes blank at the "Approve" step. (The same reason the webOS Archive community
uses an [OAuth broker](https://github.com/webOSArchive/oauth-broker-for-webos)
for other apps.)

An auth key sidesteps the browser entirely: the device proves who it is with
the key, and Tailscale's control plane — which the device *can* reach, over
TLS 1.3 — does the rest.

You only need the key **once**. After the device registers, its identity lives
in its own state file and the key is never consulted again.

---

## Part 2 — Get the key onto the TouchPad

Typing 50 random characters on a touchscreen is miserable. Pick whichever suits
you:

**A. Via USB (recommended).** Connect the TouchPad, choose **USB Drive** mode,
and save the key as a plain text file named `tailscale-authkey.txt` at the top
level of the drive. Then leave the **Auth key** box *empty* in Part 4 and the
agent will read it from there.

**B. E-mail it to yourself**, open it on the device, and paste into the Auth
key box.

**C. Type it in.** It works, it's just tedious.

With option A you can delete the file once you've connected successfully.

---

## Part 3 — Install the agent

### Option A (recommended): the `.ipk`

Grab `ipks/org.webosarchive.tailscale_1.78.1_all.ipk` from this repo, copy it to
the device, and install it with **Preware** or **WebOS Quick Install**.

> **Not `palm-install`.** The package's `postinst` has to run as root to place
> the agent in `/usr/lib/vpn/agents/`, and `palm-install` doesn't run it.

The package installs a small info app (which is just these instructions) plus
the agent itself. It's ~9 MB packaged, ~23 MB installed — nearly all of that is
the Tailscale binary.

### Option B: novacom install script (for development)

With the device connected over novacom:

```sh
cd agent-tailscale
make                      # build the ~21 KB agent plugin
./install-tailscale-agent.sh
```

See [`agent-tailscale/BUILD.md`](agent-tailscale/BUILD.md) for the toolchain and
for how to build the Tailscale binary itself.

---

## Part 4 — Create the profile on the device

**Settings → VPN → Add Profile**

| Field | What to enter |
|---|---|
| **Connection Type** | Tailscale |
| **VPN Server** | anything, e.g. `tailscale` |
| **Auth key** | your `tskey-auth-…`, or leave blank to use the file from Part 2 |
| **Device name** | defaults to the TouchPad's hostname; this is how it appears in your tailnet |
| **Login server** | blank for Tailscale; your server's URL for Headscale |
| **Exit node** | blank for a split tunnel (see below) |
| **Accept subnet routes** | leave ticked |
| **Use Tailscale DNS** | leave ticked |

Then **Save & Connect**.

### About that "VPN Server" box

webOS insists on a hostname or IP before it will let you continue, but Tailscale
has no such concept — it finds its own servers. **Whatever you type is ignored**;
it's only the label you'll see in the profile list. The agent can't pre-fill it,
because the stock Add-Profile screen populates that field before it has even
loaded the list of agents.

Headscale users: put your server's URL in the **Login server** field on the next
screen — that's the one the agent actually reads.

### Split tunnel vs. exit node

- **Exit node blank** (default): only traffic to your tailnet goes through the
  tunnel. Everything else uses the local network directly. This is the normal
  Tailscale behaviour and what you usually want.
- **Exit node set** to a peer's name or `100.x` address: *all* traffic is routed
  through that peer, like a traditional VPN. Useful on untrusted Wi-Fi.

---

## Verifying & troubleshooting

Once connected, the device appears in your admin console with a `100.x` address,
and you can reach other tailnet machines by name.

**Log on the device:** `/var/log/webos-tailscale-agent.log` — it carries both the
agent's lines and the connection script's.

| Symptom | Cause / fix |
|---|---|
| "Auth key missing, expired, or already used" | Non-reusable keys are single-use, and keys expire (90 days max). Generate a new one. |
| Connects, but private hostnames don't resolve | Tick **Use Tailscale DNS**. That points the device's resolver at Tailscale, so lookups follow the tunnel. |
| Connects, but can't reach a LAN behind a subnet router | Tick **Accept subnet routes**, and make sure the route is approved in the admin console. |
| Exit node selected and everything stops working | Reverse-path filtering. The agent sets it to loose mode automatically; if you've hardened `rp_filter` yourself, that's the culprit. |
| No Tailscale option in Settings → VPN | The `.ipk` was installed without running `postinst` (i.e. with `palm-install`). Reinstall via Preware. |
| Device shows the old name in your tailnet | The name is stored in the profile. Edit the profile and change **Device name**. |

---

## Known limitations

- **No auto-connect at boot.** Reconnect from Settings → VPN after a restart.
- **You must supply an auth key.** Browser-based sign-in can't work on this
  device (see Part 1).
- **IPv6 is not available.** webOS 3.0.5's kernel has no IPv6 stack, so the
  tunnel is IPv4-only. Tailscale detects this and carries on.
- **Reinstalling the app resets the node's identity**, since its state lives in
  the app directory. You'll need to authenticate again, and a stale entry will be
  left in your admin console to delete.
- **Tested on the HP TouchPad (webOS 3.0.5) only.** Other webOS devices have a
  different VPN app and are untested.

---

## How it performs

Better than you would expect from 2011 hardware, because WireGuard is cheap:

- **ChaCha20-Poly1305: ~146 Mbit/s** on a single core — far above what the
  device's Wi-Fi can carry, so encryption is never the bottleneck.
- **Exit-node overhead ~17%** on a real download (a 10 MB file took 19.5 s
  direct, 23.6 s through an exit node on the other side of the country).
- **Memory: 45–75 MB.**
- **NAT traversal works.** Connections to peers on the same LAN are direct in a
  few milliseconds; connections across the internet start on a relay and then
  upgrade to a direct path, exactly as Tailscale does everywhere else.

---

## How it works (for the curious)

webOS has a **pluggable VPN agent** model: `PmVpnDaemon` scans
`/usr/lib/vpn/agents/*/` for a `vpn-plugin-info.json` manifest and `dlopen`s the
plugin it names. The agent declares its own form fields, and the stock VPN app
renders whatever it's given. So a new protocol needs **no patch** to either the
daemon or the app — the same mechanism the OpenVPN agent uses.

The Tailscale agent is a ~21 KB C plugin that spawns one child process, a shell
script owning `tailscaled` plus `tailscale up`, and translates that script's
output into the connection states the UI displays.

The interesting part is that **Tailscale runs at all**. It's written in Go, and
Go officially requires a Linux kernel of 3.2 or newer — the TouchPad runs
2.6.35. That floor turns out to be policy rather than a hard requirement: a
static Go binary (`CGO_ENABLED=0`) runs fine, and because it's static it also
sidesteps the device's 2008-era glibc and its unusable OpenSSL 0.9.8k, carrying
its own TLS 1.3 instead. That's why this agent needs no supporting packages
while the OpenVPN one does.

Full technical notes, including the several ways this can go wrong on a kernel
this old, are in [`agent-tailscale/BUILD.md`](agent-tailscale/BUILD.md).
