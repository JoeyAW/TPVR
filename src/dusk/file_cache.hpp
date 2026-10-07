#pragma once

#include "dolphin/types.h"

#include <cstdint>

class JKRDvdFile;

namespace dusk::file_cache {

class Capture {
public:
    Capture(JKRDvdFile* file, bool enabled);
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

private:
    std::uint64_t mKey = 0;
    bool mActive = false;
};

bool try_read(JKRDvdFile* file, void* buffer, s32 length, s32 offset, s32& result);
void record_read(JKRDvdFile* file, const void* buffer, s32 length, s32 offset, s32 result);
s32 read_prio(JKRDvdFile* file, void* buffer, s32 length, s32 offset, s32 priority);

} // namespace dusk::file_cache
