#pragma once
// Host tools only: map a PE file from disk the way the loader would (each section at its RVA),
// so the scanners and the signature table run against StarCitizen.exe without the game.
// No relocations are applied and no code runs; addresses inside the buffer are only compared.
#include "sco/scan.h"
#include <cstdint>
#include <string>
#include <vector>

namespace sco {

struct FileImage {
    std::vector<uint8_t> mem;     // SizeOfImage bytes, sections copied to their RVAs
    Image                img;     // points into mem
    uint64_t             preferredBase = 0;
    std::string          error;   // set when Load fails
    bool Load(const std::string& path);
    uint32_t Rva(const void* p) const { return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - mem.data()); }
};

}  // namespace sco
