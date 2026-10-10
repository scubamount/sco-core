#pragma once
// Addresses behind sc-offline's world features: build mode (free camera, ground ray, the entity
// move/rotate slots and the camera fields), missions, the console and quantum cvars, and the new
// quantum drive's boost hooks. Resolved by sco::ResolveAll(); the rows are in
// src/game/world_sigs.cpp, and docs/game/world.md lists each one with what it checks.
//
// Rows are grouped into capabilities like sco/game/features.h: a feature asks for its group with
// sco::caps::SetFromSignatures(c.name, c.rows, c.count) and runs only when it's ready.
//
// Rows only find addresses. Hooking and writing stay in the product.
#include <cstddef>
#include <cstdint>

namespace sco::game::world {

struct Capability {
    const char*        name;    // "build.free_cam"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of these features, in a fixed order.
const Capability* Capabilities(size_t& count);

// build.free_cam_on / build.free_cam_off: the FreeCamEnable / FreeCamDisable console handlers
//   (the one registration of each name; +0x23 / +0x11 load "Enabling free cam" / "Disabling free
//   cam"). build.free_cam_flag: the byte FreeCamDisable tests (cmp byte [rip+X], 0 at +4).
// build.ray_tag: the "PlanetRayIntersection" string in .rdata (the ray's tag, passed to the game).
// build.ground_ray: the one LEA of that tag inside the planet ray cast; build.phys_world: the
//   physical world global it loads at -0xBF; build.release_grid: the grid release it calls at +0xF4.
// build.entity_vtable: the entity vtable whose slots 0x208, 0x2B0, 0x2C0, 0x2C8 and 0x430 hold the
//   functions below (found from the unique get-rotation function, checked slot by slot).
// build.entity_set_position (0x2B0), build.entity_set_rotation (0x2C0), build.entity_get_rotation
//   (0x2C8), build.entity_ray_proxy (0x208), build.entity_skip_add (0x430): what those slots hold;
//   sc-offline compares a live entity's slots with them.
constexpr size_t kEntitySetPositionSlot = 0x2B0;
constexpr size_t kEntitySetRotationSlot = 0x2C0;
constexpr size_t kEntityGetRotationSlot = 0x2C8;
constexpr size_t kEntityRayProxySlot    = 0x208;
constexpr size_t kEntitySkipAddSlot     = 0x430;
// build.camera_fields: teleport.to_camera + 0x17A, where the game reads the camera it teleports to
//   (actor + 0x208, checked by teleport.to_camera at +0x151). The checks pin the field offsets:
//   position (three doubles) at kCameraPosition, rotation (x, y, z, w floats) at kCameraRotation.
constexpr size_t kCameraPosition = 0x6D20;   // 4.10.196.36804 (0x6D18 before)
constexpr size_t kCameraRotation = 0x6D38;   // 4.10.196.36804 (0x6D30 before)

// missions.settings: the mission settings global checked before the "[EVMissionManager] Spawn
//   Mission Request" log line (sc-offline sets its +0xC flag).
// missions.load_all: the mission_load_all console handler; missions.subsumption: the global it
//   loads at +0x2E. missions.file_change: Subsumption::XmlFileLibrary::OnFileChange;
//   missions.xml_system: the system global it loads at +0x203.
// missions.create: the dgs.subsumption.mission.create handler.

// cvars.console: the console global the "debugGUI_enable 1" calls load (all kConsoleSites agree).
constexpr int kConsoleSites = 7;   // 4.10.196.36804
// cvars.quantum_travel_allowed .. cvars.target_lock_linear: the storage each quantum cvar
//   registers (the one registration of each name; int slot 0x40, float slot 0x48).

// quantum.on_action, quantum.start_use, quantum.drive_input, quantum.effect_update,
// quantum.charge, quantum.spline_get_y, quantum.handle_valid: the boost hooks' functions;
// quantum.audio_system: the audio system global. One unique pattern each.

}  // namespace sco::game::world
