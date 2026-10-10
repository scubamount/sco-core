/*
 * abi_titanlink.c: pins sc_titanlink.h, the MIT layout of the TitanLink bridge channel.
 *
 * Compile-only, like abi_ipc.c. sc-offline's titanlink built-in and the Northstar plugin (its own
 * repository) each build against a copy of this header from the SDK, so any line failing here is
 * a wire change: it needs TL_LAYOUT_VERSION bumped and docs/ipc.md ("Bridge layouts") updated in
 * the same change. The numbers are the ones sc-offline's titanlink_wire.h pinned before the
 * header moved here (layout version 1).
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_titanlink rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_titanlink.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

/* ---- identity and regions ---- */
PIN(sizeof(TL_CHANNEL_NAME) == 5); /* "link" */
PIN(TL_LAYOUT_ID == 0x314B4C54u);
PIN(TL_LAYOUT_VERSION == 1u);
PIN(TL_CHANNEL_BYTES == 0x10000u);
PIN(TL_OFF_SC_STATE == 0x0100u);
PIN(TL_OFF_TF_STATE == 0x0400u);
PIN(TL_OFF_FRAME == 0x0600u);
PIN(TL_OFF_TO_PEER == 0x1000u);
PIN(TL_RING_TO_PEER == 0x4000u);
PIN(TL_OFF_FROM_PEER == 0x5100u);
PIN(TL_RING_FROM_PEER == 0x4000u);
#ifdef __cplusplus
/* C11 takes no floating comparison in a constant expression; C++ checks the exact values. */
PIN(TL_UNITS_PER_METRE == 39.37);
PIN(TL_CHANNEL_NAME[0] == 'l' && TL_CHANNEL_NAME[1] == 'i' && TL_CHANNEL_NAME[2] == 'n' &&
    TL_CHANNEL_NAME[3] == 'k');
#endif

/* ---- flags, buttons, key modes ---- */
PIN(TL_SC_PILOT == 0x1u);
PIN(TL_SC_FOCUSED == 0x2u);
PIN(TL_SC_ANCHORED == 0x4u);
PIN(TL_BTN_ATTACK == 0x001u);
PIN(TL_BTN_ZOOM == 0x002u);
PIN(TL_BTN_RELOAD == 0x004u);
PIN(TL_BTN_MELEE == 0x008u);
PIN(TL_BTN_OFFHAND == 0x010u);
PIN(TL_BTN_ABILITY == 0x020u);
PIN(TL_BTN_TITAN_C == 0x040u);
PIN(TL_BTN_TITAN_V == 0x080u);
PIN(TL_BTN_CYCLE == 0x100u);
PIN(TL_BTN_X == 0x200u);
PIN(TL_TF_IN_MATCH == 0x1u);
PIN(TL_TF_IN_TITAN == 0x2u);
PIN(TL_TF_PARKED == 0x4u);
PIN(TL_TF_BUSY == 0x8u);
PIN(TL_TF_FLAGS == 0xFu);
PIN(TL_KEY_NONE == 0u);
PIN(TL_KEY_DEPTH == 1u);
PIN(TL_KEY_BRIGHT == 2u);
PIN(TL_KEY_GUN == 3u);

/* ---- ring messages ---- */
PIN(TL_MSG_START_MATCH == 1u);
PIN(TL_MSG_CALL_TITAN == 2u);
PIN(TL_MSG_EMBARK == 3u);
PIN(TL_MSG_DISEMBARK == 4u);
PIN(TL_MSG_PILOT_OFF == 5u);
PIN(TL_MSG_LOG == 1u);
PIN(TL_MSG_LOG_MAX == 200u);

/* ---- tl_sc_state ---- */
SIZE(tl_sc_state, 80);
AT(tl_sc_state, time_ms, 0);
AT(tl_sc_state, flags, 8);
AT(tl_sc_state, buttons, 12);
AT(tl_sc_state, eye, 16);
AT(tl_sc_state, ang, 28);
AT(tl_sc_state, feet, 40);
AT(tl_sc_state, vel, 52);
AT(tl_sc_state, fov_scale, 64);
AT(tl_sc_state, view_w, 68);
AT(tl_sc_state, view_h, 72);
AT(tl_sc_state, reserved, 76);

/* ---- tl_tf_state ---- */
SIZE(tl_tf_state, 124);
AT(tl_tf_state, flags, 0);
AT(tl_tf_state, pid, 4);
AT(tl_tf_state, origin, 8);
AT(tl_tf_state, eye_z, 20);
AT(tl_tf_state, titan_origin, 24);
AT(tl_tf_state, titan_yaw, 36);
AT(tl_tf_state, clip, 40);
AT(tl_tf_state, shots, 44);
AT(tl_tf_state, damage, 48);
AT(tl_tf_state, titan_health, 52);
AT(tl_tf_state, floor_pt, 56);
AT(tl_tf_state, zoom_frac, 68);
AT(tl_tf_state, zoom_fov, 72);
AT(tl_tf_state, weapon, 76);

/* ---- tl_frame ---- */
SIZE(tl_frame, 136);
AT(tl_frame, handle, 0);
AT(tl_frame, tex_seq, 16);
AT(tl_frame, width, 24);
AT(tl_frame, height, 28);
AT(tl_frame, key_mode, 32);
AT(tl_frame, presents, 36);
AT(tl_frame, status, 40);

/* ---- message payloads ---- */
SIZE(tl_msg_start_match, 96);
AT(tl_msg_start_match, map, 0);
AT(tl_msg_start_match, mode, 64);
SIZE(tl_msg_call_titan, 16);
AT(tl_msg_call_titan, drop, 0);
AT(tl_msg_call_titan, yaw, 12);

/* Keeps the translation unit non-empty under -Wpedantic. */
void sco_abi_titanlink_pins(void);
void sco_abi_titanlink_pins(void) {}
