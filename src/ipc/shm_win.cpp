// sco.ipc on Windows: a pagefile-backed named section in the session's Local\ namespace whose
// security descriptor names the current user as owner and grants that user alone access. An
// existing section of the same name is used only if its owner is the current user and it is
// large enough, so another account's squatter can't hand us its memory.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include "shm.h"
#include <vector>

namespace sco::ipc::shm {

namespace {

// The current user's SID, copied out of the process token.
bool UserSid(std::vector<BYTE>& out) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> info(n ? n : 1);
    const bool ok = n && GetTokenInformation(token, TokenUser, info.data(), n, &n);
    CloseHandle(token);
    if (!ok) return false;
    PSID sid = reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid;
    const DWORD len = GetLengthSid(sid);
    out.resize(len);
    return CopySid(len, out.data(), sid) != FALSE;
}

std::string Error(const char* what) { return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")"; }

}  // namespace

Result Create(const std::string& name, uint64_t bytes, Mapping& out, std::string& why) {
    out = Mapping{};
    std::vector<BYTE> sidBuf;
    if (!UserSid(sidBuf)) {
        why = Error("reading the current user");
        return Result::Failed;
    }
    PSID sid = sidBuf.data();
    const DWORD aclBytes = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid));
    std::vector<BYTE> aclBuf(aclBytes);
    PACL acl = reinterpret_cast<PACL>(aclBuf.data());
    SECURITY_DESCRIPTOR sd;
    if (!InitializeAcl(acl, aclBytes, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, FILE_MAP_ALL_ACCESS, sid) ||
        !InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorOwner(&sd, sid, FALSE)) {
        why = Error("building the security descriptor");
        return Result::Failed;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    const std::wstring wname(name.begin(), name.end());   // ASCII: the service built it
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, static_cast<DWORD>(bytes >> 32),
                                  static_cast<DWORD>(bytes & 0xFFFFFFFFu), wname.c_str());
    const DWORD created = GetLastError();
    if (!h) {
        why = Error("CreateFileMapping");
        return Result::Failed;
    }
    const bool existed = created == ERROR_ALREADY_EXISTS;
    if (existed) {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR got = nullptr;
        const DWORD rc = GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &got);
        const bool mine = rc == ERROR_SUCCESS && owner && EqualSid(owner, sid);
        if (got) LocalFree(got);
        if (!mine) {
            CloseHandle(h);
            why = "a mapping of that name already exists and belongs to another account";
            return Result::Failed;
        }
    }
    void* base = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base) {
        why = Error("MapViewOfFile");
        CloseHandle(h);
        return Result::Failed;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < bytes) {
        why = "a mapping of that name already exists and is smaller";
        UnmapViewOfFile(base);
        CloseHandle(h);
        return Result::Failed;
    }
    out.base = base;
    out.bytes = bytes;
    out.handle = reinterpret_cast<uintptr_t>(h);
    out.existed = existed;
    out.name = name;
    return Result::Ok;
}

void Close(Mapping& m) {
    if (m.base) UnmapViewOfFile(m.base);
    if (m.handle) CloseHandle(reinterpret_cast<HANDLE>(m.handle));
    m = Mapping{};
}

uint64_t NowMs() { return GetTickCount64(); }

uint32_t Pid() { return GetCurrentProcessId(); }

}  // namespace sco::ipc::shm
