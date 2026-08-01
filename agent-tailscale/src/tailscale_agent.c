/*
 * tailscale_agent.c  --  webOS VPN agent plugin that drives Tailscale.
 *
 * Makes the stock Settings -> VPN app configure and run a Tailscale session,
 * using the reverse-engineered agent ABI (see webos_vpn_agent_abi.h). No
 * patching of PmVpnDaemon or com.palm.app.vpn -- discovery is manifest-driven
 * and the form is data-driven. Structure deliberately mirrors openvpn_agent.c.
 *
 * Flow:
 *   initVpnAgent            -> fill descriptor + capture host callback table.
 *   connect(new profile)    -> launch the stock configure-profile scene with a
 *                              Tailscale field schema (host0 = send-msg-to-app);
 *                              reply -7 (silent in every scene).
 *   connect(saved profile)  -> write tailscale.conf from the profile fields,
 *                              spawn scripts/tailscale-run (ONE child that owns
 *                              tailscaled + `tailscale up`), ACK success, then
 *                              push state via host1 as the child's output
 *                              crosses milestones.
 *   disconnect              -> SIGTERM the runner (it does `tailscale down`,
 *                              `tailscaled --cleanup`, DNS restore), reply only
 *                              once it has actually exited.
 *
 * Unlike OpenVPN there is no per-connection credential prompt: Tailscale
 * authenticates with a pre-generated auth key (tskey-...) stored in the profile.
 * Interactive browser login can't work on-device (the 2009 WebView can't render
 * a modern consent screen), so if `tailscale up` returns a login URL we surface
 * it as an error telling the user to paste a key instead.
 *
 * Build: ../Makefile + ../BUILD.md. Links libcjson (json-c) + libglib-2.0.
 */
#include "webos_vpn_agent_abi.h"

#include <json.h>        /* Palm json-c: json_object_*  (link: -lcjson) */
#include <glib_shim.h>   /* GLib main-loop subset  (link: -lglib-2.0)  */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define AGENT_ID    "org.webosarchive.tailscale"
#define AGENT_LOG   "/var/log/webos-tailscale-agent.log"
#define STATE_DIR   "/tmp/webos-tailscale"
#define CONF_FILE    STATE_DIR "/tailscale.conf"
#define RUN_SCRIPT  "/usr/lib/vpn/agents/tailscale/tailscale-run"

/* Typing a 50-character tskey on a touchscreen is miserable, so the auth key may
 * instead be dropped in a file. `tailscale --auth-key` natively understands a
 * "file:<path>" argument, so we just hand the path through.
 *
 * If the Auth key field is left BLANK we look here; if the field contains a bare
 * path (starts with '/') we use that. Copy the key over USB drive mode -- the
 * media partition mounts as a normal USB disk, so the file lands here with no
 * typing at all. */
#define AUTHKEY_FILE "/media/internal/tailscale-authkey.txt"

/* Response codes -- see the ABI header. -7 is the ONLY code the app treats as
 * silent in EVERY scene, so it's what we use after launching the profile form. */
#define RSP_OK            0
#define RSP_NEED_CREDS   (-7)
#define RSP_ERR          (-1)

/* ------------------------------------------------------------------ *
 *  Logging
 * ------------------------------------------------------------------ */
static void agent_log(const char *fmt, ...)
{
    FILE *f = fopen(AGENT_LOG, "a");
    if (!f) return;
    /* Keep the log owner-only: op_connect params can include profile details.
     * (Cheap enough to re-assert on every write; the file is opened rarely.) */
    chmod(AGENT_LOG, 0600);
    time_t t = time(NULL);
    char ts[32];
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(f, "%s [%s] ", ts, AGENT_ID);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------ *
 *  Global agent state
 * ------------------------------------------------------------------ */
static host_send_msg_fn    g_send_msg;    /* host0: launch/relaunch VPN app  */
static host_notify_fn      g_notify;      /* host1: push status to getStatus */
static host_get_profile_fn g_get_profile; /* host11: load+decrypt a profile   */

/* Touched ONLY from the daemon's main thread (ops + GLib watch callbacks), so
 * no locking and no cross-thread host calls -- host0/host1 are main-thread-only. */
static pid_t       g_run_pid  = -1;
static guint       g_io_tag   = 0;
static GIOChannel *g_run_chan = NULL;
static char  g_profile_name[128] = "";
static char  g_state[32] = "disconnected";
static char  g_last_error[256] = "";

/* A disconnect whose reply we defer until the runner actually exits (replying
 * early makes the VPN app show a spurious "Disconnect Failure"). */
static vpn_response_cb g_disc_cb    = NULL;
static void           *g_disc_token = NULL;

/* ------------------------------------------------------------------ *
 *  Small json helpers
 * ------------------------------------------------------------------ */
static struct json_object *jobj_get(struct json_object *o, const char *key)
{
    struct json_object *v = NULL;
    if (o && json_object_object_get_ex(o, key, &v))
        return v;
    return NULL;
}

static const char *jstr(struct json_object *o, const char *key, const char *dflt)
{
    struct json_object *v = jobj_get(o, key);
    if (v && json_object_is_type(v, json_type_string))
        return json_object_get_string(v);
    return dflt;
}

/* The "value" json_object of a vpnFormFields entry, by field id. */
static struct json_object *field_obj(struct json_object *fields, const char *id)
{
    if (!fields || !json_object_is_type(fields, json_type_array))
        return NULL;
    int n = json_object_array_length(fields);
    for (int i = 0; i < n; i++) {
        struct json_object *f = json_object_array_get_idx(fields, i);
        const char *fid = jstr(f, "id", NULL);
        if (fid && strcmp(fid, id) == 0)
            return jobj_get(f, "value");
    }
    return NULL;
}

static const char *field_value(struct json_object *fields, const char *id,
                               const char *dflt)
{
    struct json_object *v = field_obj(fields, id);
    if (v && json_object_is_type(v, json_type_string))
        return json_object_get_string(v);
    return dflt;
}

/* Checkboxes may come back as a real boolean, an int, or a string depending on
 * how DynamicForm round-trips them -- accept all three. */
static int field_bool(struct json_object *fields, const char *id, int dflt)
{
    struct json_object *v = field_obj(fields, id);
    if (!v) return dflt;
    if (json_object_is_type(v, json_type_boolean))
        return json_object_get_boolean(v) ? 1 : 0;
    if (json_object_is_type(v, json_type_int))
        return json_object_get_int(v) != 0;
    if (json_object_is_type(v, json_type_string)) {
        const char *s = json_object_get_string(v);
        if (!s || !*s) return dflt;
        return (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                strcmp(s, "on")   == 0 || strcmp(s, "yes") == 0);
    }
    return dflt;
}

/* ------------------------------------------------------------------ *
 *  Field schema (Tailscale). Consumed by com.palm.app.vpn/DynamicForm.js.
 *   { id, type: textfield|passwordfield|listselector|checkbox,
 *     label, value, editable, options:[{label,value}] }
 * ------------------------------------------------------------------ */
static struct json_object *mk_text(const char *id, const char *label,
                                   const char *value)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "id",       json_object_new_string(id));
    json_object_object_add(o, "type",     json_object_new_string("textfield"));
    json_object_object_add(o, "label",    json_object_new_string(label));
    json_object_object_add(o, "value",    json_object_new_string(value ? value : ""));
    json_object_object_add(o, "editable", json_object_new_boolean(1));
    return o;
}

static struct json_object *mk_pass(const char *id, const char *label,
                                   const char *value)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "id",       json_object_new_string(id));
    json_object_object_add(o, "type",     json_object_new_string("passwordfield"));
    json_object_object_add(o, "label",    json_object_new_string(label));
    json_object_object_add(o, "value",    json_object_new_string(value ? value : ""));
    json_object_object_add(o, "editable", json_object_new_boolean(1));
    return o;
}

/* NB: the checkbox value must be the STRING "true"/"false", not a JSON boolean.
 * DynamicForm.js renders it as
 *     checked: (data.value === "true") ? true : false
 * -- a strict === against a string. A JSON boolean therefore never matches, so
 * the box silently renders UNCHECKED regardless of the default we asked for, and
 * is then saved back as "false" (it writes the same string form out again). That
 * is why profiles came back with accept-routes/accept-dns off. */
static struct json_object *mk_check(const char *id, const char *label, int on)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "id",       json_object_new_string(id));
    json_object_object_add(o, "type",     json_object_new_string("checkbox"));
    json_object_object_add(o, "label",    json_object_new_string(label));
    json_object_object_add(o, "value",    json_object_new_string(on ? "true" : "false"));
    json_object_object_add(o, "editable", json_object_new_boolean(1));
    return o;
}

/* Default device name = the machine's actual hostname. webOS sets a real one
 * (/etc/hostname, e.g. "FreshPad"), so honour it rather than inventing a generic
 * label -- that way the node shows up in the tailnet under the name the owner
 * already knows the device by. Tailscale lowercases/sanitises it for DNS itself. */
static const char *default_hostname(void)
{
    static char buf[96];
    if (buf[0]) return buf;
    if (gethostname(buf, sizeof buf - 1) != 0 || !buf[0])
        snprintf(buf, sizeof buf, "webos-touchpad");
    buf[sizeof buf - 1] = 0;
    return buf;
}

/* `seed` (may be NULL) is the existing vpnFormFields, so re-editing a saved
 * profile keeps its values. */
static struct json_object *build_form(struct json_object *seed)
{
    struct json_object *fields = json_object_new_array();

    /* Generated in the Tailscale admin console under Settings -> Keys (or
     * `headscale preauthkeys create`). Interactive browser login is not viable
     * on this device, so a key is how we authenticate. Leave BLANK to read it
     * from AUTHKEY_FILE instead -- far kinder than typing 50 characters on a
     * touchscreen (see resolve_authkey). */
    json_object_array_add(fields,
        mk_pass("tsAuthKey", "Auth key (blank = use key file)",
                field_value(seed, "tsAuthKey", "")));

    json_object_array_add(fields,
        mk_text("tsHostname", "Device name",
                field_value(seed, "tsHostname", default_hostname())));

    /* Blank = Tailscale's own control plane. Set for headscale, e.g.
     * https://headscale.example.com  (the Add-Profile "Server" field is also
     * honoured as a shortcut -- see effective_login_server()). */
    json_object_array_add(fields,
        mk_text("tsLoginServer", "Login server (blank = Tailscale)",
                field_value(seed, "tsLoginServer", "")));

    /* Blank = split tunnel (tailnet traffic only). Set to a peer's tailnet IP or
     * name to route ALL traffic through it. */
    json_object_array_add(fields,
        mk_text("tsExitNode", "Exit node (blank = off)",
                field_value(seed, "tsExitNode", "")));

    json_object_array_add(fields,
        mk_check("tsAcceptRoutes", "Accept subnet routes",
                 field_bool(seed, "tsAcceptRoutes", 1)));

    json_object_array_add(fields,
        mk_check("tsAcceptDns", "Use Tailscale DNS",
                 field_bool(seed, "tsAcceptDns", 1)));

    return fields;
}

/* ------------------------------------------------------------------ *
 *  host0: launch the configure-profile scene with our field schema.
 * ------------------------------------------------------------------ */
static void prompt_for_profile(const char *profile_name, const char *host,
                               struct json_object *seed_fields)
{
    if (!g_send_msg) { agent_log("prompt: no host0!"); return; }

    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "vpnAgentGuid", json_object_new_string(AGENT_ID));
    json_object_object_add(msg, "vpnMsgType",   json_object_new_string("credentials"));
    if (profile_name && *profile_name)
        json_object_object_add(msg, "vpnProfileName",
                               json_object_new_string(profile_name));
    if (host && *host)
        json_object_object_add(msg, "vpnHost", json_object_new_string(host));
    json_object_object_add(msg, "vpnFormFields", build_form(seed_fields));

    const char *s = json_object_to_json_string(msg);
    agent_log("prompt_for_profile: %s", s);
    g_send_msg(s);
    json_object_put(msg);
}

/* ------------------------------------------------------------------ *
 *  host1: push a connection-state update to getStatus subscribers.
 * ------------------------------------------------------------------ */
static void notify_state(const char *state)
{
    snprintf(g_state, sizeof g_state, "%s", state);

    if (!g_notify) return;
    struct json_object *o = json_object_new_object();
    if (*g_profile_name)
        json_object_object_add(o, "vpnProfileName",
                               json_object_new_string(g_profile_name));
    json_object_object_add(o, "state", json_object_new_string(state));
    const char *s = json_object_to_json_string(o);
    agent_log("notify_state: %s", s);
    g_notify(s);
    json_object_put(o);
}

/* ------------------------------------------------------------------ *
 *  host0: show an error banner in the VPN app.
 *
 *  VpnApp.js handleLaunch: when the launch params carry `banner` (and a
 *  vpnAgentGuid) it opens BannerDialog with that string as the message --
 *  checked BEFORE popupPrompt/vpnFormFields, so a banner never opens the
 *  profile form by accident. This is our only way to tell the user *why*
 *  a connection failed; otherwise the UI silently returns to "disconnected".
 * ------------------------------------------------------------------ */
static void show_banner(const char *msg)
{
    if (!g_send_msg || !msg || !*msg) return;

    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "vpnAgentGuid", json_object_new_string(AGENT_ID));
    json_object_object_add(o, "banner",       json_object_new_string(msg));
    if (*g_profile_name)
        json_object_object_add(o, "vpnProfileName",
                               json_object_new_string(g_profile_name));
    const char *s = json_object_to_json_string(o);
    agent_log("show_banner: %s", s);
    g_send_msg(s);
    json_object_put(o);
}

/* ------------------------------------------------------------------ *
 *  Config generation (a /bin/sh-sourceable file for tailscale-run)
 * ------------------------------------------------------------------ */
static int file_exists(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0;
}

/* Emit KEY='value' with POSIX-correct single-quote escaping ( ' -> '\'' ). */
static void put_shell_var(FILE *f, const char *key, const char *val)
{
    fprintf(f, "%s='", key);
    for (const char *p = val ? val : ""; *p; p++) {
        if (*p == '\'') fputs("'\\''", f);
        else            fputc(*p, f);
    }
    fputs("'\n", f);
}

/* The stock Add-Profile scene REQUIRES a "VPN Server" (hostname or IP) and won't
 * let you continue without one -- but Tailscale has no such concept: the client
 * finds its control plane on its own. We deliberately IGNORE vpnHost so the user
 * can type literally anything there, and treat the explicit "Login server" field
 * as the sole authority (blank = Tailscale's own control plane, or set it to a
 * headscale URL).
 *
 * We can't pre-fill the Server box from here: AddProfile.js sets it from its own
 * `vpnServerName` property during create(), before the agent list has even
 * loaded, and nothing routes agent data into it. Changing that would mean
 * patching com.palm.app.vpn, which this whole design avoids.
 *
 * An earlier version promoted a non-Tailscale vpnHost to https://<host> as a
 * headscale shortcut -- that made an arbitrary placeholder ("vpn", "1.2.3.4")
 * silently become a bogus control server, which is far worse than ignoring it.
 * A bare scheme-less value is still accepted in the Login server field below. */
static void effective_login_server(const char *host, struct json_object *fields,
                                   char *out, size_t outsz)
{
    const char *explicit_ls = field_value(fields, "tsLoginServer", "");
    out[0] = 0;

    if (host && *host)
        agent_log("effective_login_server: ignoring vpnHost='%s' "
                  "(Tailscale needs no server; use the Login server field)", host);

    if (!explicit_ls || !*explicit_ls)
        return;                      /* blank -> Tailscale's control plane */

    if (strncmp(explicit_ls, "http://", 7) == 0 ||
        strncmp(explicit_ls, "https://", 8) == 0)
        snprintf(out, outsz, "%s", explicit_ls);
    else
        snprintf(out, outsz, "https://%s", explicit_ls);   /* be forgiving */
}

/* Resolve the Auth key field into whatever `tailscale --auth-key` should get:
 * the literal key, or a "file:<path>" reference. Returns 0 if nothing usable. */
static int resolve_authkey(struct json_object *fields, char *out, size_t outsz)
{
    const char *k = field_value(fields, "tsAuthKey", "");
    out[0] = 0;

    if (k && *k) {
        if (strncmp(k, "file:", 5) == 0) {          /* already a file ref */
            snprintf(out, outsz, "%s", k);
        } else if (k[0] == '/') {                   /* bare path -> file ref */
            snprintf(out, outsz, "file:%s", k);
        } else {                                    /* literal tskey-... */
            snprintf(out, outsz, "%s", k);
        }
        return 1;
    }
    /* Field blank: fall back to the well-known file if it exists. */
    if (file_exists(AUTHKEY_FILE)) {
        snprintf(out, outsz, "file:%s", AUTHKEY_FILE);
        agent_log("resolve_authkey: field blank -> using %s", AUTHKEY_FILE);
        return 1;
    }
    return 0;
}

static int write_config(const char *host, struct json_object *fields)
{
    mkdir(STATE_DIR, 0700);

    char authkey[512];
    resolve_authkey(fields, authkey, sizeof authkey);
    const char *hostname = field_value(fields, "tsHostname", default_hostname());
    const char *exitnode = field_value(fields, "tsExitNode", "");
    int accept_routes    = field_bool(fields, "tsAcceptRoutes", 1);
    int accept_dns       = field_bool(fields, "tsAcceptDns", 1);

    char login_server[256];
    effective_login_server(host, fields, login_server, sizeof login_server);

    FILE *f = fopen(CONF_FILE, "w");
    if (!f) {
        agent_log("write_config: cannot open %s: %s", CONF_FILE, strerror(errno));
        return -1;
    }
    fprintf(f, "# generated by %s -- do not edit\n", AGENT_ID);
    put_shell_var(f, "TS_AUTHKEY",      authkey);
    put_shell_var(f, "TS_HOSTNAME",     hostname && *hostname ? hostname : default_hostname());
    put_shell_var(f, "TS_LOGIN_SERVER", login_server);
    put_shell_var(f, "TS_EXIT_NODE",    exitnode);
    fprintf(f, "TS_ACCEPT_ROUTES=%d\n", accept_routes ? 1 : 0);
    fprintf(f, "TS_ACCEPT_DNS=%d\n",    accept_dns ? 1 : 0);
    fclose(f);
    chmod(CONF_FILE, 0600);          /* the auth key is a credential */

    /* Never log the key itself -- only whether we have one and in what form. */
    agent_log("write_config: wrote %s (login_server=%s exit=%s routes=%d dns=%d key=%s)",
              CONF_FILE, login_server, exitnode, accept_routes, accept_dns,
              !*authkey ? "NONE" :
              (strncmp(authkey, "file:", 5) == 0 ? "file-ref" : "literal"));
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Runner process: spawn + monitor
 * ------------------------------------------------------------------ */

/* Map one line of runner output onto a connection state.
 *
 * ONLY the runner's explicit "TSAGENT-STATE:" markers are authoritative.
 *
 * We deliberately do NOT interpret tailscaled's own "Switching ipn state" lines.
 * That was a real bug: tailscaled's normal startup emits
 *   "Switching ipn state NoState -> Stopped (WantRunning=false, nm=false)"
 * a beat BEFORE `tailscale up` has taken effect. Treating "-> Stopped" as
 * "disconnected" made the agent report failure immediately, whereupon the daemon
 * tore the session down and called cleanupVpnAgent -> kill_runner, SIGTERMing
 * the runner ~4s in and killing every connect attempt from the UI. The runner
 * already knows the true state (it waits for `up` to return), so trust it. */
/* Answer a disconnect request that we parked until teardown finished.
 *
 * Teardown is "done" when the RUNNER says so, not when its process object is
 * reaped: as soon as we report "disconnected" the daemon deactivates the plugin
 * and calls cleanupVpnAgent, so the g_child_watch for the runner never gets a
 * chance to fire. Waiting for on_run_exit therefore meant the Luna reply was
 * never sent at all and the VPN app showed a disconnect error -- even though the
 * tunnel had in fact come down cleanly. */
static void answer_deferred_disconnect(void)
{
    if (!g_disc_cb) return;
    agent_log("answer_deferred_disconnect: replying success");
    g_disc_cb(g_disc_token, 0, RSP_OK, NULL);
    g_disc_cb = NULL;
    g_disc_token = NULL;
}

static void handle_line(gchar *line, gsize len)
{
    if (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[len - 1] = 0;
    agent_log("run: %s", line);

    const char *p;
    if ((p = strstr(line, "TSAGENT-STATE:")) != NULL) {
        p += strlen("TSAGENT-STATE:");
        while (*p == ' ') p++;
        if (*p) {
            notify_state(p);
            /* The runner emits this only after `tailscale down`, the ip-rule
             * removal and the DNS restore have all completed, so it is a true
             * teardown-complete signal (~2s after SIGTERM in practice). */
            if (strcmp(p, "disconnected") == 0)
                answer_deferred_disconnect();
        }
        return;
    }
    if ((p = strstr(line, "TSAGENT-ERROR:")) != NULL) {
        p += strlen("TSAGENT-ERROR:");
        while (*p == ' ') p++;
        snprintf(g_last_error, sizeof g_last_error, "%s", p);
        agent_log("ERROR reported by runner: %s", g_last_error);
        /* Tell the user WHY. Without this the UI just slides back to
         * "disconnected" with no explanation of what went wrong. */
        show_banner(g_last_error);
        return;
    }
}

static void drain_channel(GIOChannel *ch)
{
    if (!ch) return;
    for (;;) {
        gchar *line = NULL;
        gsize len = 0;
        GIOStatus st = g_io_channel_read_line(ch, &line, &len, NULL, NULL);
        if (st == G_IO_STATUS_NORMAL && line) { handle_line(line, len); g_free(line); continue; }
        if (line) g_free(line);
        break;   /* AGAIN (no full line yet), EOF, or ERROR */
    }
}

static gboolean on_run_output(GIOChannel *ch, GIOCondition cond, gpointer d)
{
    drain_channel(ch);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        g_io_tag = 0;          /* returning FALSE removes this source */
        return FALSE;
    }
    return TRUE;
}

static void on_run_exit(GPid pid, gint status, gpointer d)
{
    drain_channel(g_run_chan);      /* capture the last lines before teardown */
    agent_log("on_run_exit: pid=%d status=%d", (int)pid, status);
    if (g_io_tag)   { g_source_remove(g_io_tag); g_io_tag = 0; }
    if (g_run_chan) { g_io_channel_unref(g_run_chan); g_run_chan = NULL; }
    g_run_pid = -1;
    notify_state("disconnected");

    /* Fallback: normally the runner's "disconnected" marker already answered
     * this. Covers the case where the runner dies without reporting. */
    answer_deferred_disconnect();
}

static int spawn_runner(void)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { agent_log("pipe: %s", strerror(errno)); return -1; }

    pid_t pid = fork();
    if (pid < 0) { agent_log("fork: %s", strerror(errno));
                   close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        /* child */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        /* Own process group, so the runner's own children (tailscaled) don't
         * receive signals aimed at the daemon. */
        setpgid(0, 0);
        execl("/bin/sh", "sh", RUN_SCRIPT, CONF_FILE, (char *)NULL);
        fprintf(stderr, "exec %s failed: %s\n", RUN_SCRIPT, strerror(errno));
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    g_run_pid = pid;

    g_run_chan = g_io_channel_unix_new(pipefd[0]);
    g_io_channel_set_close_on_unref(g_run_chan, TRUE);
    g_io_tag = g_io_add_watch(g_run_chan, G_IO_IN | G_IO_HUP | G_IO_ERR,
                              on_run_output, NULL);
    g_child_watch_add((GPid)pid, on_run_exit, NULL);

    agent_log("spawn_runner: pid=%d", pid);
    return 0;
}

/* Ask the runner to tear down; on_run_exit does the bookkeeping. */
static void kill_runner(void)
{
    if (g_run_pid > 0) {
        agent_log("kill_runner: SIGTERM %d", (int)g_run_pid);
        kill(g_run_pid, SIGTERM);
    }
}

/* ------------------------------------------------------------------ *
 *  Ops (invoked by PmVpnDaemon) -- see the ABI header: the signatures are
 *  NOT uniform, and arg1 for connect/handle_ui is a raw JSON *string*.
 * ------------------------------------------------------------------ */
#define JC_IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4000)

static struct json_object *parse_params(const char *params_json)
{
    if (!params_json) return NULL;
    struct json_object *o = json_tokener_parse(params_json);
    if (JC_IS_ERR(o)) return NULL;
    return o;
}

struct profile_capture { char *json; };
static void profile_load_cb(const char *json, void *ud)
{
    struct profile_capture *c = ud;
    if (json && !c->json) c->json = strdup(json);
}

static int fields_ok(struct json_object *f)
{
    return f && json_object_is_type(f, json_type_array) &&
           json_object_array_length(f) > 0;
}

static void op_connect(void *token, const char *params_json, vpn_response_cb cb)
{
    agent_log("op_connect: params=%s", params_json ? params_json : "(null)");
    struct json_object *params = parse_params(params_json);
    struct json_object *loaded = NULL;

    const char *pname = jstr(params, "vpnProfileName", "");
    struct json_object *prof   = jobj_get(params, "vpnProfile");
    struct json_object *fields = jobj_get(prof, "vpnFormFields");

    /* The app connects a SAVED profile by name only; fetch it via host11. */
    if (!fields_ok(fields) && *pname && g_get_profile) {
        struct profile_capture cap = { NULL };
        agent_log("op_connect: loading saved profile '%s' via host11", pname);
        g_get_profile(pname, profile_load_cb, &cap);
        if (cap.json) {
            loaded = json_tokener_parse(cap.json);
            if (JC_IS_ERR(loaded)) loaded = NULL;
            free(cap.json);
            struct json_object *lprof = jobj_get(loaded, "vpnProfile");
            prof = lprof ? lprof : loaded;
            fields = jobj_get(prof, "vpnFormFields");
        } else {
            agent_log("op_connect: host11 returned no profile for '%s'", pname);
        }
    }

    const char *host = jstr(prof, "vpnHost", jstr(params, "vpnHost", ""));

    /* Truly new profile -> launch the configure scene. */
    if (!fields_ok(fields)) {
        agent_log("op_connect: no fields -> prompting for profile");
        prompt_for_profile(pname, host, NULL);
        if (cb) cb(token, 0, RSP_NEED_CREDS, NULL);   /* silent in app */
        goto done;
    }

    /* A saved profile with no auth key can't authenticate: interactive login
     * needs a browser the device doesn't have. Re-open the form instead of
     * failing obscurely. A blank field is fine if AUTHKEY_FILE exists. */
    {
        char probe[512];
        if (!resolve_authkey(fields, probe, sizeof probe)) {
            agent_log("op_connect: no auth key (and no %s) -> re-prompting",
                      AUTHKEY_FILE);
            prompt_for_profile(pname, host, fields);
            if (cb) cb(token, 0, RSP_NEED_CREDS, NULL);
            goto done;
        }
    }

    snprintf(g_profile_name, sizeof g_profile_name, "%s", pname);
    g_last_error[0] = 0;

    if (!file_exists(RUN_SCRIPT)) {
        agent_log("op_connect: runner missing at %s", RUN_SCRIPT);
        if (cb) cb(token, 0, RSP_ERR, "Tailscale agent files are missing");
        goto done;
    }
    if (write_config(host, fields) != 0) {
        if (cb) cb(token, 0, RSP_ERR, "Failed to write Tailscale config");
        goto done;
    }

    notify_state("connecting");
    if (spawn_runner() != 0) {
        notify_state("disconnected");
        if (cb) cb(token, 0, RSP_ERR, "Failed to start Tailscale");
        goto done;
    }
    if (cb) cb(token, 0, RSP_OK, NULL);   /* ACK; state follows via notify */

done:
    if (loaded) json_object_put(loaded);
    if (params) json_object_put(params);
}

/* ABI: op(token, cb) -- cb is arg1, NOT arg2. Reading it from the wrong slot
 * jumps through garbage and SIGSEGVs the daemon mid-teardown. */
static void op_disconnect(void *token, vpn_response_cb cb)
{
    agent_log("op_disconnect: dispatched");
    if (g_run_pid > 0) {
        /* Reply only once teardown actually completes (like vpnc). */
        g_disc_cb = cb;
        g_disc_token = token;
        notify_state("disconnecting");
        kill_runner();
    } else {
        notify_state("disconnected");
        if (cb) cb(token, 0, RSP_OK, NULL);
    }
}

/* Same convention as disconnect: op(token, cb). */
static void op_get_connection_details(void *token, vpn_response_cb cb)
{
    /* Just ACK -- the daemon already tracks state from our notify_state() calls;
     * broadcasting here creates a notify->poll->notify feedback loop. */
    if (cb) cb(token, 0, RSP_OK, NULL);
}

static void op_handle_ui_prompt_response(void *token, const char *params_json,
                                         vpn_response_cb cb)
{
    agent_log("op_handle_ui_prompt_response: %s",
              params_json ? params_json : "(null)");
    struct json_object *params = parse_params(params_json);
    const char *btn = jstr(params, "buttonId", "");
    if (strcmp(btn, "backButton") == 0) {
        kill_runner();
        notify_state("disconnected");
    }
    if (cb) cb(token, 0, RSP_OK, NULL);
    if (params) json_object_put(params);
}

/* Fire-and-forget; arg convention unconfirmed, so take only token and invoke no
 * callback (a wrongly-positioned cb would SIGSEGV the daemon). tailscaled has
 * its own link monitor and re-establishes on network changes. */
static void op_notify_system_change(void *token)
{
    (void)token;
    agent_log("op_notify_system_change: dispatched");
}

/* ------------------------------------------------------------------ *
 *  Exported entry points
 * ------------------------------------------------------------------ */
#define VPN_EXPORT __attribute__((visibility("default")))

VPN_EXPORT
int initVpnAgent(VpnAgentDescriptor *desc,
                 host_send_msg_fn host_send_msg,
                 host_notify_fn   host_notify,
                 void *host_add_iface,
                 void *host3, void *host4, void *host5, void *host6,
                 void *host7, void *host8, void *host9, void *host10,
                 host_get_profile_fn host_get_profile)
{
    (void)host_add_iface; (void)host3; (void)host4; (void)host5; (void)host6;
    (void)host7; (void)host8; (void)host9; (void)host10;

    g_send_msg    = host_send_msg;
    g_notify      = host_notify;
    g_get_profile = host_get_profile;

    agent_log("initVpnAgent: desc=%p host0=%p host1=%p host11=%p",
              (void *)desc, (void *)host_send_msg, (void *)host_notify,
              (void *)host_get_profile);
    if (!desc) return 1;   /* nonzero => failure (daemon unloads us) */

    memset(desc, 0, sizeof *desc);      /* 60 bytes; daemon owns 0x3c.. */
    desc->version = 1;
    strncpy(desc->id, AGENT_ID, sizeof desc->id - 1);

    desc->connect                = op_connect;
    desc->disconnect             = (vpn_op_fn)op_disconnect;
    desc->get_connection_details = (vpn_op_fn)op_get_connection_details;
    desc->handle_ui_prompt_resp  = op_handle_ui_prompt_response;
    desc->notify_system_change   = (vpn_op_fn)op_notify_system_change;

    agent_log("initVpnAgent: registered id=%s", desc->id);
    return 0;
}

VPN_EXPORT
void cleanupVpnAgent(void)
{
    agent_log("cleanupVpnAgent: called");
    /* Last resort: the daemon is about to unload us, so any still-parked
     * disconnect reply would be lost forever (the app would report a disconnect
     * failure for a tunnel that actually came down fine). Answer it now. */
    answer_deferred_disconnect();
    kill_runner();
}
