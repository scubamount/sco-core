// SPDX-License-Identifier: GPL-3.0-only
#include "ipc/diagnostics_audit.h"
#include "ipc/diagnostics_ipc.h"
#include <cstdio>
#include <cstdlib>

using namespace sco::diagnostics;

#define CHECK(x) do { \
    if (!(x)) { \
        std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        std::abort(); \
    } \
} while (0)

void TestProtectedAndAcknowledged() {
    // 1. Valid Accepted receipt
    audit::Receipt accepted{audit::Status::Accepted, 100, true, true};
    CHECK(audit::Protected(accepted, 200, 101));
    CHECK(audit::Acknowledged(accepted, 200, 101));

    // 2. Valid Verified receipt
    audit::Receipt verified{audit::Status::Verified, 100, true, true};
    CHECK(audit::Protected(verified, 200, 101));
    // Verified is protected, but NOT acknowledged (Acknowledged requires Status::Accepted)
    CHECK(!audit::Acknowledged(verified, 200, 101));

    // 3. Time / Boundary Checks
    // observed == completedMs is valid (completedMs <= observed)
    CHECK(audit::Protected(accepted, 200, 100));
    CHECK(audit::Acknowledged(accepted, 200, 100));

    // observed < completedMs is invalid
    CHECK(!audit::Protected(accepted, 200, 99));
    CHECK(!audit::Acknowledged(accepted, 200, 99));

    // observed >= deadline is invalid (observed < deadline required)
    CHECK(!audit::Protected(accepted, 200, 200));
    CHECK(!audit::Acknowledged(accepted, 200, 200));
    CHECK(!audit::Protected(accepted, 200, 201));
    CHECK(!audit::Acknowledged(accepted, 200, 201));

    // completedMs == 0 is invalid
    audit::Receipt zeroTime = accepted;
    zeroTime.completedMs = 0;
    CHECK(!audit::Protected(zeroTime, 200, 101));
    CHECK(!audit::Acknowledged(zeroTime, 200, 101));

    // 4. Flags Checks
    audit::Receipt noSink = accepted;
    noSink.protectedSink = false;
    CHECK(!audit::Protected(noSink, 200, 101));
    CHECK(!audit::Acknowledged(noSink, 200, 101));

    audit::Receipt noIdentity = accepted;
    noIdentity.effectiveIdentity = false;
    CHECK(!audit::Protected(noIdentity, 200, 101));
    CHECK(!audit::Acknowledged(noIdentity, 200, 101));

    // 5. Failure Statuses
    for (auto status : {audit::Status::Unavailable, audit::Status::IdentityDenied,
                        audit::Status::ProtectionDenied, audit::Status::Deadline,
                        audit::Status::Full, audit::Status::Invalid, audit::Status::Busy}) {
        audit::Receipt failR = accepted;
        failR.status = status;
        CHECK(!audit::Protected(failR, 200, 101));
        CHECK(!audit::Acknowledged(failR, 200, 101));
    }
}

void TestCollectorFallback() {
#ifndef _WIN32
    audit::WindowsOptions options{L"SCOFixtureAudit", L"SCOFixtureSource", 65536};
    auto collector = audit::OpenWindowsCollector(options);
    CHECK(collector != nullptr);

    // Verify should return Unavailable on non-Windows
    auto verifyReceipt = collector->Verify(1000);
    CHECK(verifyReceipt.status == audit::Status::Unavailable);
    CHECK(!verifyReceipt.protectedSink);
    CHECK(!verifyReceipt.effectiveIdentity);

    // Append should return Unavailable on non-Windows
    ipc::AuditEvent event{};
    audit::Coordinates coords{};
    auto appendReceipt = collector->Append(event, coords, 1000);
    CHECK(appendReceipt.status == audit::Status::Unavailable);
    CHECK(!appendReceipt.protectedSink);
    CHECK(!appendReceipt.effectiveIdentity);
#endif
}

int main() {
    TestProtectedAndAcknowledged();
    TestCollectorFallback();
    std::puts("PASS: diagnostics_audit unit tests");
    return 0;
}
