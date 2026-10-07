#pragma once
#include <windows.h>
#include "../../shared/bridge.h"

namespace gml::shm {

inline Header*& Ptr() { static Header* p = nullptr; return p; }
inline Header* Get() { return Ptr(); }

inline bool Create() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  DWORD(kShmSize >> 32), DWORD(kShmSize & 0xffffffff), kShmName);
    if (!h) return false;
    auto* hdr = static_cast<Header*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, kShmSize));
    if (!hdr) return false;
    ZeroMemory(hdr, sizeof(Header));
    hdr->version = kVersion;
    hdr->dlPid = GetCurrentProcessId();
    hdr->frame.latest = ~0u;
    hdr->frame.reading = ~0u;
    MemoryBarrier();
    hdr->magic = kMagic;
    Ptr() = hdr;
    return true;
}

// Single producer (DL window thread).
inline void Push(uint32_t type, int32_t a, int32_t b = 0) {
    Header* hdr = Get();
    if (!hdr) return;
    InputQueue& q = hdr->input;
    uint32_t head = q.head;
    if (head - q.tail >= kMaxInputEvents) return;  // GMod not keeping up; drop
    q.events[head % kMaxInputEvents] = {type, a, b};
    MemoryBarrier();
    q.head = head + 1;
}

}  // namespace gml::shm
