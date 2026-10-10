/*
 * abi_voxel_bridge.c: pins sc_voxel_bridge.h, the MIT layout of the voxel-game bridge channel.
 *
 * Compile-only, like abi_ipc.c. sc-offline's voxel_bridge built-in and the voxel game's mod each
 * build against a copy of this header from the SDK, so any line failing here is a wire change: it
 * needs VX_LAYOUT_VERSION bumped and docs/ipc.md ("Bridge layouts") updated in the same change.
 * The numbers are the ones sc-offline's voxel_wire.h pinned before the header moved here (layout
 * version 1).
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_voxel_bridge rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_voxel_bridge.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

/* ---- identity, regions, coordinate limits ---- */
PIN(sizeof(VX_CHANNEL_NAME) == 5); /* "link" */
PIN(VX_LAYOUT_ID == 0x31425856u);
PIN(VX_LAYOUT_VERSION == 1u);
PIN(VX_CHANNEL_BYTES == 0x800000u);
PIN(VX_OFF_SC_STATE == 0x0100u);
PIN(VX_OFF_PEER_STATE == 0x0200u);
PIN(VX_OFF_TO_PEER == 0x1000u);
PIN(VX_RING_TO_PEER == 0x100000u);
PIN(VX_OFF_FROM_PEER == 0x101100u);
PIN(VX_RING_FROM_PEER == 0x400000u);
PIN((int)VX_BASE_Y == 64);
PIN(VX_MAX_XZ == 30000);
PIN(VX_MIN_Y == -64);
PIN(VX_MAX_Y == 320);
#ifdef __cplusplus
/* C11 takes no floating comparison in a constant expression; C++ checks the exact values. */
PIN(VX_BASE_Y == 64.0);
PIN(VX_CHANNEL_NAME[0] == 'l' && VX_CHANNEL_NAME[1] == 'i' && VX_CHANNEL_NAME[2] == 'n' &&
    VX_CHANNEL_NAME[3] == 'k');
#endif

/* ---- flags ---- */
PIN(VX_SC_SPAWNED == 0x1u);
PIN(VX_SC_FOCUSED == 0x2u);
PIN(VX_SC_ANCHORED == 0x4u);
PIN(VX_SC_PAUSED == 0x8u);
PIN(VX_PEER_IN_WORLD == 0x1u);
PIN(VX_PEER_FLAGS == 0x1u);

/* ---- ring messages ---- */
PIN(VX_MSG_AREA == 1u);
PIN(VX_MSG_GROUND == 2u);
PIN(VX_MSG_SOLIDS == 1u);
PIN(VX_MSG_CLEAR == 2u);
PIN(VX_MSG_LOG == 3u);
PIN(VX_MSG_LOG_MAX == 200u);

/* ---- vx_sc_state ---- */
SIZE(vx_sc_state, 64);
AT(vx_sc_state, time_ms, 0);
AT(vx_sc_state, flags, 8);
AT(vx_sc_state, epoch, 12);
AT(vx_sc_state, feet, 16);
AT(vx_sc_state, yaw, 40);
AT(vx_sc_state, pitch, 44);
AT(vx_sc_state, block_size, 48);
AT(vx_sc_state, view_w, 52);
AT(vx_sc_state, view_h, 56);
AT(vx_sc_state, reserved, 60);

/* ---- vx_peer_state ---- */
SIZE(vx_peer_state, 96);
AT(vx_peer_state, flags, 0);
AT(vx_peer_state, epoch, 4);
AT(vx_peer_state, feet, 8);
AT(vx_peer_state, status, 32);

/* ---- message payloads ---- */
SIZE(vx_msg_area, 8);
AT(vx_msg_area, epoch, 0);
AT(vx_msg_area, reserved, 4);
SIZE(vx_msg_ground, 16);
AT(vx_msg_ground, x0, 0);
AT(vx_msg_ground, z0, 4);
AT(vx_msg_ground, epoch, 8);
AT(vx_msg_ground, count, 12);
SIZE(vx_ground, 16);
AT(vx_ground, x, 0);
AT(vx_ground, z, 4);
AT(vx_ground, top, 8);
AT(vx_ground, reserved, 12);
SIZE(vx_msg_solids, 528);
AT(vx_msg_solids, sx, 0);
AT(vx_msg_solids, sy, 4);
AT(vx_msg_solids, sz, 8);
AT(vx_msg_solids, epoch, 12);
AT(vx_msg_solids, bits, 16);

/* Keeps the translation unit non-empty under -Wpedantic. */
void sco_abi_voxel_bridge_pins(void);
void sco_abi_voxel_bridge_pins(void) {}
