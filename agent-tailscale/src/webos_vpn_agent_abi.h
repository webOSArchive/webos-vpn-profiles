/*
 * webos_vpn_agent_abi.h
 *
 * Reverse-engineered ABI for webOS pluggable VPN agents, recovered from BOTH
 * sides of the interface on an HP TouchPad (webOS 3.0.5):
 *   - agent side : /usr/lib/vpn/agents/vpnc/libVpncAgent.so   (unstripped, C)
 *   - host  side : /usr/bin/PmVpnDaemon                        (unstripped, C)
 * See BUILD.md / ../reversing for the disassembly this was read from.
 *
 * CONFIDENCE (all HIGH unless noted): every offset/signature below was read
 * straight out of one or both binaries -- descriptor fill in initVpnAgent, the
 * dlopen/dlsym/init call site in PmVpnDaemon::activatePlugin, the op invocation
 * sites (handleLuna*Request), and the host callback table at daemon .data
 * 0x21988.
 */
#ifndef WEBOS_VPN_AGENT_ABI_H
#define WEBOS_VPN_AGENT_ABI_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Request/response protocol (per op call)
 *
 * PmVpnDaemon dispatches each Luna request to the active plugin by calling one
 * of the five op function pointers in the descriptor with THREE args:
 *
 *     op(void *token, const char *params_json, vpn_response_cb cb)
 *
 *   ** THE OP SIGNATURE IS NOT UNIFORM ** (verified at the PmVpnDaemon call
 *   sites). Only connect and handle_ui_prompt_response take a params json; the
 *   others put the callback in the SECOND arg with no params:
 *       connect                (0x28): (token, params_json, cb)   cb = arg2
 *       disconnect             (0x2c): (token, cb)                cb = arg1
 *       get_connection_details (0x30): (token, cb)                cb = arg1
 *       handle_ui_prompt_resp  (0x34): (token, params_json, cb)   cb = arg2
 *       notify_system_change   (0x38): (token) [treat as no-reply]
 *   Reading cb from the wrong arg and calling it jumps through garbage (r2/r3
 *   are left uninitialised by the daemon) and SIGSEGVs it -- this is exactly
 *   what made disconnect crash the daemon mid-teardown. Define each op with its
 *   real signature and cast into the (uniform) descriptor slot type.
 *
 *   token  : opaque per-request handle owned by the daemon. Pass it back
 *            verbatim to cb. (At the call site it is [req+4]; the daemon uses
 *            it to route the Luna reply.)
 *   params_json : the request payload as a raw JSON *string* (the daemon
 *            g_strdup's the LSMessage payload). The agent must parse it itself
 *            with json_tokener_parse (exactly as vpnc_connect does). May be
 *            NULL. NB: parsing garbage-as-object segfaults the daemon.
 *   cb     : the daemon's response callback (e.g. handleLunaCmdConnectResponseCb
 *            -> handleComplexCmdResponseCb). Signature recovered exactly:
 *
 *     cb(void *token, int zero, int code, const char *errText)
 *
 *   zero    : always 0 (the stock cmd_request_send_response hardwires it).
 *   code    : 0 = success (returnValue:true). Negative = failure/soft-cancel.
 *             SILENT (no error dialog) codes DIFFER BY SCENE: AddProfile / Main /
 *             ConfigureProfile suppress -5 and -7; the CredsPrompt modal
 *             suppresses -6 and -7. **-7 is the only code silent in ALL of them**
 *             -- use it when you've just launched the profile/credentials form
 *             and aren't really failing. (Returning -6 to the add/configure flow
 *             pops "Connection Failure: undefined" AND deletes a new profile.)
 *   errText : human-readable error string, or NULL. Becomes errorText.
 *
 * The connect ACK carries no payload; real connection state is delivered
 * asynchronously through the host "notify" callback (see below).
 * ------------------------------------------------------------------------ */
struct json_object; /* from <json.h> (libcjson.so) */

typedef void (*vpn_response_cb)(void *token, int zero, int code,
                                const char *err_text);

typedef void (*vpn_op_fn)(void *token, const char *params_json,
                          vpn_response_cb cb);

/* --------------------------------------------------------------------------
 * The agent descriptor.
 *
 * PmVpnDaemon::activatePlugin g_try_malloc0's a 72-byte plugin struct, stores
 * the dlsym'd initVpnAgent/cleanupVpnAgent/handle at +0x3c/+0x40/+0x44, then
 * calls initVpnAgent(pluginStruct, host0, host1, host2, ...). The agent fills
 * the FIRST 60 bytes (memset 60) -- exactly up to +0x3c -- so it must not
 * touch anything at/after 0x3c. Offsets read from initVpnAgent:
 *
 *   0x00 version = 1
 *   0x04 id[32]  (strncpy, agent guid)
 *   0x24 pad
 *   0x28 connect
 *   0x2c disconnect
 *   0x30 get_connection_details
 *   0x34 handle_ui_prompt_response
 *   0x38 notify_system_change
 *   ---- sizeof filled region == 0x3c (60)  (daemon owns 0x3c..0x47)
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t  version;                 /* 0x00 */
    char      id[32];                  /* 0x04 */
    uint8_t   _pad[4];                 /* 0x24 */
    vpn_op_fn connect;                 /* 0x28 */
    vpn_op_fn disconnect;              /* 0x2c */
    vpn_op_fn get_connection_details;  /* 0x30 */
    vpn_op_fn handle_ui_prompt_resp;   /* 0x34 */
    vpn_op_fn notify_system_change;    /* 0x38 */
} VpnAgentDescriptor;

/* --------------------------------------------------------------------------
 * Host callback table (agent -> daemon).
 *
 * initVpnAgent is called as (desc, host0, host1, host2, host3, ...): three
 * pointers in r1..r3 and the rest on the stack, matching the daemon's static
 * table at .data 0x21988. Identified against PmVpnDaemon's symbol table:
 *
 *   [0] handleLunaServiceSendMsgToAppCb   int (*)(const char *json)  -> prompt
 *   [1] handleLunaServiceNotifyCb         int (*)(const char *json)  -> status
 *   [2] addNetworkInterface                                          (M2 route)
 *   [3] modifyNetworkInterface
 *   [4] removeNetworkInterface
 *   [5] getNetworkInterfaces
 *   [6] updateIpTableRules
 *   [7] (unnamed)  [8] (unnamed)  [9] (unnamed)  [10] (unnamed)
 *   [11] profileGetPrvDetails
 *
 * host0/host1 both take a single JSON *string* (the daemon g_strdup's it and
 * schedules a task). host0's string becomes the VPN app's launch params
 * (enyo.windowParams); host1's string is pushed to getStatus subscribers.
 *
 * We only bind the two we use in M1/M2 (host0=send-to-app, host1=notify) plus
 * the network-interface helpers we may want later; the trailing ones are kept
 * as opaque void* so the calling convention still lines up.
 * ------------------------------------------------------------------------ */
typedef int (*host_send_msg_fn)(const char *json_str);   /* host0 */
typedef int (*host_notify_fn)(const char *json_str);     /* host1 */

/* host11 = profileGetPrvDetails(name, cb, userdata): looks the profile up in the
 * daemon DB, DECRYPTS it, and synchronously calls cb(profileJsonString, ud).
 * This is how the agent gets a saved profile's fields on a name-only connect --
 * the daemon does NOT expand the profile into the connect params (confirmed on
 * device: op_connect gets only {vpnProfileName, vpnAgentGuid}). Signature read
 * from PmVpnDaemon::profileGetPrvDetails @ 0xbd5c. */
typedef void (*host_profile_cb)(const char *profile_json, void *userdata);
typedef void (*host_get_profile_fn)(const char *name, host_profile_cb cb,
                                    void *userdata);

/* Compile-time guards (32-bit ARM target only). */
#define WEBOS_CONCAT_(a, b) a##b
#define WEBOS_CONCAT(a, b)  WEBOS_CONCAT_(a, b)
#define WEBOS_ABI_ASSERT(cond) \
    typedef char WEBOS_CONCAT(webos_abi_assert_, __LINE__)[(cond) ? 1 : -1]

#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 4)
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, version)                == 0x00);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, id)                     == 0x04);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, connect)                == 0x28);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, disconnect)             == 0x2c);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, get_connection_details) == 0x30);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, handle_ui_prompt_resp)  == 0x34);
WEBOS_ABI_ASSERT(offsetof(VpnAgentDescriptor, notify_system_change)   == 0x38);
WEBOS_ABI_ASSERT(sizeof(VpnAgentDescriptor)                           == 0x3c);
#endif

/* Exported entry points dlsym'd by PmVpnDaemon. Must be default-visibility.
 * initVpnAgent receives the descriptor + the host callback table (3 regs +
 * stack). We declare enough trailing void* to capture the ones we use. */
/* NOTE: initVpnAgent MUST return int 0. PmVpnDaemon::activatePlugin checks the
 * return value (cmp r0,#0; bne <unload>) and dlclose()s the plugin if it is
 * nonzero -- so a `void` init leaves garbage in r0 and the agent gets unloaded
 * before any op is dispatched. The stock vpnc agent ends with `mov r0,#0`. */
int  initVpnAgent(VpnAgentDescriptor *desc,
                  host_send_msg_fn host_send_msg,   /* host0 */
                  host_notify_fn   host_notify,     /* host1 */
                  void *host_add_iface,             /* host2 */
                  void *host3, void *host4, void *host5, void *host6,
                  void *host7, void *host8, void *host9, void *host10,
                  host_get_profile_fn host_get_profile);  /* host11 */
void cleanupVpnAgent(void);

#endif /* WEBOS_VPN_AGENT_ABI_H */
