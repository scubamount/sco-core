// SPDX-License-Identifier: GPL-3.0-only
#include <sc_cvars_write.h>
#include "../src/game/services/cvars.h"
#include "sco/host.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include "sco/plugins.h"
#include "sco/caps.h"
#include "../src/api/internal.h"
namespace C = sco::game::services;
using sco::Result;
static int pass = 0, fail = 0;
#define CHECK(x) do { if (x) ++pass; else { ++fail; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static uint32_t values[4] = {1, 0, 1, 0};
static bool hook_ready = false;
namespace sco::game::services { bool SpawnReleaseHooked() { return hook_ready; } }
static unsigned writes = 0;
static sco_result write_result = SCO_OK;
static bool ignore_write = false;
static sco_plugin* release_during_read = nullptr;
static sco_plugin* cleanup_during_write = nullptr;
static unsigned reads = 0;
static sco_result Read(void*, uint32_t i, uint32_t* out) {
    ++reads;
    if (release_during_read) { auto* owner = release_during_read; release_during_read = nullptr; sco::Release(owner); }
    *out = values[i]; return SCO_OK;
}
static sco_result Write(void*, uint32_t i, uint32_t value) {
    ++writes;
    if (cleanup_during_write) { auto* owner = cleanup_during_write; cleanup_during_write = nullptr; C::ReleaseCvarsOwner(owner); }
    if (!ignore_write) {
        values[i] = value;
    }
    return write_result;
}
int main(int argc, char** argv) {
    sco::SetGameThread(); auto* self = sco::host::NewPlugin("cvars_writer");
    const auto* api = sco::host::BuildApi({"synthetic cvars writer"});
    C::CvarsPort port{}; port.verified = true; port.offline_admitted = true; port.read_bool = Read;
    port.write_verified = true; port.write_bool = Write;
    CHECK(C::SetCvarsPort(port) == Result::Ok);
    CHECK(C::StartCvars() == Result::Ok);
    const void* raw = nullptr;
    CHECK(api->query_service(SC_CVARS_WRITE_NAME, SC_CVARS_WRITE_VERSION, &raw) == SCO_OK);
    auto* t = static_cast<const sc_cvars_write_v1*>(raw);
    CHECK(t && t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_UNAVAILABLE);
    C::StopCvars();
    // Portable stand-in for spawn.cpp's existing dispatcher; actual runtime release.
    CHECK(sco::AddReleaseHook(C::ReleaseCvarsOwner) == Result::Ok); hook_ready = true;
    CHECK(C::StartCvars() == Result::Ok);
    auto* abandoned = sco::host::NewPlugin("cvars_abandoned");
    const unsigned before_release = writes;
    release_during_read = abandoned;
    CHECK(t->set_bool(abandoned, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_BAD_ARG);
    CHECK(writes == before_release && C::PendingCvarRestores() == 0);
    const unsigned before_invalid = writes;
    CHECK(t->set_bool(self, "g_squadron_frontend 1", SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->set_bool(self, "net_enable", SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->set_bool(self, nullptr, SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->set_bool(self, "g_squadron_frontend", 2, 1) == SCO_BAD_ARG);
    CHECK(t->set_bool(self, "g_squadron_frontend", SC_CVAR_BOOL, 2) == SCO_BAD_ARG);
    CHECK(t->set_bool(nullptr, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->set_bool(reinterpret_cast<sco_plugin*>(1), "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->restore_all(nullptr) == SCO_BAD_ARG);
    std::thread worker([&] {
        CHECK(t->set_bool(self, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_WRONG_THREAD);
        CHECK(t->restore_all(self) == SCO_WRONG_THREAD);
        CHECK(C::StopCvars() == Result::WrongThread);
    }); worker.join();
    CHECK(writes == before_invalid);
    sco::caps::Set("game.cvars.write", false, "synthetic absent capability");
    CHECK(t->set_bool(self, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    sco::caps::Set("game.cvars.write", true, "synthetic");
    sco::detail::SetRuntimeStopping(true);
    CHECK(t->set_bool(self, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    sco::detail::SetRuntimeStopping(false);
    CHECK(t && t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    CHECK(values[0] == 0);
    sco::caps::Set("game.cvars.read", false, "synthetic read capability loss");
    CHECK(t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    CHECK(values[0] == 0);
    CHECK(t->restore_all(self) == SCO_OK); // cleanup ignores capability loss
    sco::caps::Set("game.cvars.read", true, "synthetic");
    CHECK(values[0] == 1);
    sco::plugins::Options opts; opts.enabled = true;
    auto modules = sco::plugins::Discover(CVARS_PLUGIN_ROOT, opts);
    CHECK(modules.size() == 1);
    auto* probe = sco::host::NewPlugin("cvars_probe");
    CHECK(sco::plugins::LoadNative(modules[0], api, probe, opts));
    CHECK(values[0] == 0);
    sco::plugins::UnloadNative(modules[0]);
    CHECK(modules[0].state == sco::plugins::State::Unloaded && values[0] == 1);
    const unsigned after_unload = writes;
    sco::plugins::UnloadNative(modules[0]);
    CHECK(writes == after_unload);
    // Actual loader retained-provider verdict: do not run ordinary resource hooks.
    auto retained_modules = sco::plugins::Discover(CVARS_PLUGIN_ROOT, opts);
    auto* retained_owner = sco::host::NewPlugin("cvars_probe");
    CHECK(sco::plugins::LoadNative(retained_modules[0], api, retained_owner, opts));
    static const uint32_t retained_table[2] = {8, 0};
    CHECK(api->provide_service(retained_owner, "cvars_probe.state", 0x00010000u, retained_table) == SCO_OK);
    const void* held = nullptr;
    CHECK(api->query_service("cvars_probe.state", 0x00010000u, &held) == SCO_OK);
    sco::plugins::MarkCrashed(retained_modules[0], "synthetic attributed provider fault", 1);
    CHECK(retained_modules[0].state == sco::plugins::State::Faulted);
    CHECK(values[0] == 0 && C::PendingCvarRestores() == 1);
    C::StopCvars(); // host/game-owned restoration independent of skipped owner hooks
    CHECK(values[0] == 1 && C::PendingCvarRestores() == 0);
    CHECK(C::StartCvars() == Result::Ok);
    CHECK(t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    CHECK(t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 1) == SCO_OK);
    CHECK(t->set_bool(self, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    auto* other = sco::host::NewPlugin("cvars_other");
    CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    CHECK(values[0] == 0);
    CHECK(t->set_bool(other, "pl_lensdisplay.minimap_enabled", SC_CVAR_BOOL, 1) == SCO_OK);
    CHECK(sco::Release(self) == Result::Ok);
    CHECK(values[0] == 1 && values[1] == 1);
    CHECK(t->set_bool(self, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_BAD_ARG);
    CHECK(t->restore_all(self) == SCO_BAD_ARG);
    CHECK(t->restore_all(other) == SCO_OK && values[1] == 0);
    CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    CHECK(t->set_bool(other, "pl_lensdisplay.minimap_enabled", SC_CVAR_BOOL, 1) == SCO_OK);
    CHECK(sco::Dispatch("game.exit", nullptr) == Result::Ok);
    CHECK(values[0] == 1 && values[1] == 0);
    CHECK(t->set_bool(other, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    C::StopCvars();
    CHECK(C::SetCvarsPort(port) == Result::Ok);
    CHECK(C::StartCvars() == Result::Ok);
    CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    C::StopCvars();
    CHECK(values[0] == 1);
    CHECK(t->set_bool(other, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    CHECK(C::StartCvars() == Result::Ok);
    if (argc > 1 && std::strcmp(argv[1], "cleanup-reentry") == 0) {
        cleanup_during_write = other;
        const unsigned before_reentry = reads;
        CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_FAILED);
        CHECK(reads == before_reentry + 1); // prior read only: no adapter reentry after latch
    } else if (argc > 1 && std::strcmp(argv[1], "write-fault") == 0) {
        write_result = SCO_FAILED;
        CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_FAILED);
    } else if (argc > 1 && std::strcmp(argv[1], "write-mismatch") == 0) {
        ignore_write = true;
        CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_FAILED);
    } else {
        CHECK(t->set_bool(other, "i_target_selector.targeting2_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
        write_result = SCO_FAILED;
        CHECK(t->restore_all(other) == SCO_FAILED);
    }
    const unsigned at_fault = writes;
    CHECK(C::PendingCvarRestores() == 1);
    CHECK(t->set_bool(other, "g_squadron_frontend", SC_CVAR_BOOL, 1) == SCO_UNAVAILABLE);
    CHECK(t->restore_all(other) == SCO_UNAVAILABLE);
    C::ReleaseCvarsOwner(other);
    CHECK(C::StopCvars() == Result::Failed);
    CHECK(C::SetCvarsPort(port) == Result::Failed);
    CHECK(C::StartCvars() == Result::Failed);
    CHECK(C::PendingCvarRestores() == 1 && writes == at_fault);
    std::printf("cvars writes: %d passed, %d failed\n", pass, fail); return fail ? 1 : 0;
}
