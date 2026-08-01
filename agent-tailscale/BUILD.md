# Building the webOS Tailscale VPN agent

Two independent artifacts:

1. **`libVpnTailscaleAgent.so`** — the ~21 KB C plugin `PmVpnDaemon` dlopens.
2. **`tailscaled`** — a ~23 MB static Go binary (Tailscale itself), cross-compiled
   for ARMv7. Not built by the Makefile; see below.

---

## 1. The agent plugin

```sh
cd agent-tailscale
make            # -> libVpnTailscaleAgent.so
make check      # confirm 32-bit ARM ELF, softfp, entry points, NEEDED
```

Toolchain: Linaro GCC 4.9.4 `arm-linux-gnueabi` at
`/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi` (override with
`TOOLCHAIN=` or `CROSS_COMPILE=`).

`sysroot/` is a symlink to `../agent-openvpn/sysroot` — the same device
`libcjson.so` / `libglib-2.0.so.0` plus the shim headers (`json.h`,
`glib_shim.h`). Nothing extra to fetch.

ABI details live in `src/webos_vpn_agent_abi.h`. The op signatures are **not
uniform** — read that header before touching the descriptor.

---

## 2. tailscaled (the Go binary)

Go cross-compiles with no sysroot and no toolchain fight:

```sh
git clone --depth 1 --branch v1.78.1 https://github.com/tailscale/tailscale.git
cd tailscale
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 \
  go build -tags ts_include_cli -ldflags="-s -w" -o tailscaled ./cmd/tailscaled
cp tailscaled ../agent-tailscale/tailscaled
```

Takes ~30 s. `agent-tailscale/tailscaled` is gitignored (too big for git).

**Why v1.78.1:** its `go.mod` pins Go 1.23.1, the last Go series that officially
supports kernels older than 3.2. In practice a **Go 1.25** build also runs fine on
the TouchPad's 2.6.35 kernel — the 3.2 floor is policy, not a hard break — but
v1.78.1 is the conservative choice. `CGO_ENABLED=0` matters: a static binary
ignores the device's ancient glibc 2.8 entirely.

**`ts_include_cli`** makes one combined binary: invoked as `tailscale`
(argv[0]) it is the CLI, as `tailscaled` it is the daemon. The installer exploits
this with a symlink so we ship only one 23 MB copy.

---

## 3. Install for development

```sh
./agent-tailscale/install-tailscale-agent.sh          # full install
SKIP_BIN=1 ./agent-tailscale/install-tailscale-agent.sh   # skip the 23 MB push
```

Run it from the **repo root** — and always confirm the deploy landed:

```sh
md5sum agent-tailscale/libVpnTailscaleAgent.so
echo 'md5sum /usr/lib/vpn/agents/tailscale/libVpnTailscaleAgent.so' | novacom run file://bin/sh
```

(A stale `.so` on the device once cost an hour of debugging a bug that had
already been fixed.)

### Layout, and why it is split

| Path | What |
|---|---|
| `/usr/lib/vpn/agents/tailscale/libVpnTailscaleAgent.so` | the plugin |
| `/usr/lib/vpn/agents/tailscale/tailscale-run` | orchestrator script |
| `/usr/lib/vpn/agents/tailscale/vpn-plugin-info.json` | manifest |
| `/usr/lib/vpn/agents/tailscale/tailscale` | **symlink** to the binary below |
| `<app dir>/agent/tailscaled` | the 23 MB Go binary |
| `<app dir>/agent/state/` | node identity (survives reconnects) |

The root filesystem has only ~120 MB free, so the big binary lives on
`/media/cryptofs`. cryptofs is fuse and **refuses symlinks**, but `/` is ext3 —
hence the CLI symlink sits on `/` and points the other way.

`/` is mounted **read-only**; the installer remounts it rw (Preware does this
itself for a real `.ipk` install).

---

## 4. Package

```sh
./packaging-tailscale/build-ipk.sh     # -> packaging-tailscale/dist/*.ipk  (~8.8 MB)
```

Install with **Preware or WebOS Quick Install**, never `palm-install` — postinst
must run as root.

---

## Debugging

Device log: `/var/log/webos-tailscale-agent.log` (mode 0600; carries both
`[org.webosarchive.tailscale]` agent lines and `[tailscale-run]` script lines).

- The `.so` is **dlopen'ed lazily**, on the first real connect. No log file
  before that is normal, and `getAgents` only reads the manifest.
- `luna-send` returns nothing from a novacom shell (LS2 role policy denies it) —
  it is not a way to test the agent.
- `ps` shows only the command name, so the runner appears as `sh`. Find it via
  `/proc/<pid>/cmdline`.
- After `killall PmVpnDaemon` the plugin reloads with fresh globals, so a running
  runner becomes orphaned — tear it down before testing disconnect.

### Traps worth knowing (each one cost real time)

- **Do not interpret tailscaled's `Switching ipn state` lines.** Its normal
  startup emits `NoState -> Stopped` *before* `tailscale up` takes effect; acting
  on that reports failure and the daemon kills the session ~4 s in.
- **Checkbox `value` must be the string `"true"`/`"false"`**, not a JSON boolean —
  `DynamicForm.js` does `checked: (data.value === "true")`.
- **Answer a deferred disconnect on the runner's `disconnected` marker**, not on
  child-process exit: the daemon unloads the plugin as soon as we report
  disconnected, so `on_run_exit` never fires and the reply is lost.
- **`tailscale up` needs `--timeout`** or it blocks forever waiting for an
  interactive login that cannot happen here.
- **`tailscaled --cleanup` does not reliably remove the ip rules** — delete prefs
  5210/5230/5250/5270 and flush table 52 by hand, or a disconnect leaves the
  device blackholing traffic.
- **`rp_filter` must be 2 (loose)** for exit-node mode; webOS ships strict (1).
