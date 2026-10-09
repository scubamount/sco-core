#ifdef _WIN32
#include <windows.h>
#include <cstring>
#include "sco/scan.h"

namespace sco {

Image ModuleImage() {
    Image img;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return img;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    img.base      = base;
    img.timestamp = nt->FileHeader.TimeDateStamp;
    img.size      = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (pdata.VirtualAddress && pdata.Size && pdata.VirtualAddress < img.size)
        img.pdata = { base + pdata.VirtualAddress, pdata.Size };
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const Section s{ base + sec->VirtualAddress, sec->Misc.VirtualSize };
        if (strncmp(reinterpret_cast<const char*>(sec->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) img.text = s;
        else if (strncmp(reinterpret_cast<const char*>(sec->Name), ".rdata", IMAGE_SIZEOF_SHORT_NAME) == 0) img.rdata = s;
    }
    return img;
}

}  // namespace sco
#endif
