#include "sco/pe_file.h"
#include <cstdio>
#include <cstring>

namespace sco {

namespace {
template <typename T> bool Get(const std::vector<uint8_t>& f, size_t off, T& out) {
    if (off + sizeof(T) > f.size()) return false;
    memcpy(&out, f.data() + off, sizeof(T));
    return true;
}
}  // namespace

bool FileImage::Load(const std::string& path) {
    std::vector<uint8_t> file;
    if (FILE* fp = fopen(path.c_str(), "rb")) {
        fseek(fp, 0, SEEK_END);
        const long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (len > 0) {
            file.resize(static_cast<size_t>(len));
            if (fread(file.data(), 1, file.size(), fp) != file.size()) file.clear();
        }
        fclose(fp);
    }
    if (file.empty()) { error = "can't read " + path; return false; }

    uint16_t mz = 0; uint32_t pe = 0, sig = 0;
    if (!Get(file, 0, mz) || mz != 0x5A4D || !Get(file, 0x3C, pe) || !Get(file, pe, sig) || sig != 0x4550) {
        error = "not a PE file"; return false;
    }
    uint16_t nsec = 0, optSize = 0, magic = 0;
    uint32_t timestamp = 0, sizeOfImage = 0, sizeOfHeaders = 0;
    uint64_t base = 0;
    if (!Get(file, pe + 6, nsec) || !Get(file, pe + 8, timestamp) || !Get(file, pe + 20, optSize) ||
        !Get(file, pe + 24, magic) || magic != 0x20B || !Get(file, pe + 24 + 24, base) ||
        !Get(file, pe + 24 + 56, sizeOfImage) || !Get(file, pe + 24 + 60, sizeOfHeaders)) {
        error = "not a 64-bit PE image"; return false;
    }
    if (sizeOfImage == 0 || sizeOfImage > 0x40000000u || sizeOfHeaders > file.size()) { error = "bad image size"; return false; }
    mem.assign(sizeOfImage, 0);
    memcpy(mem.data(), file.data(), sizeOfHeaders < sizeOfImage ? sizeOfHeaders : sizeOfImage);

    img = {};
    img.base = mem.data();
    img.timestamp = timestamp;
    img.size = sizeOfImage;
    preferredBase = base;
    const size_t secTable = pe + 24 + optSize;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t o = secTable + 40u * i;
        if (o + 40 > file.size()) { error = "truncated section table"; return false; }
        char name[9] = {};
        memcpy(name, file.data() + o, 8);
        uint32_t vsize = 0, va = 0, rawSize = 0, rawPtr = 0;
        Get(file, o + 8, vsize); Get(file, o + 12, va); Get(file, o + 16, rawSize); Get(file, o + 20, rawPtr);
        if (va >= sizeOfImage) { error = std::string("section outside the image: ") + name; return false; }
        const uint32_t room = sizeOfImage - va;
        uint32_t copy = rawSize < vsize ? rawSize : vsize;
        if (copy > room) copy = room;
        if (rawPtr > file.size() || copy > file.size() - rawPtr) { error = std::string("section past end of file: ") + name; return false; }
        memcpy(mem.data() + va, file.data() + rawPtr, copy);
        const Section s{ mem.data() + va, vsize < room ? vsize : room };
        if (strcmp(name, ".text") == 0) img.text = s;
        else if (strcmp(name, ".rdata") == 0) img.rdata = s;
    }
    if (!img.text.base || !img.rdata.base) { error = "no .text or .rdata section"; return false; }
    return true;
}

}  // namespace sco
