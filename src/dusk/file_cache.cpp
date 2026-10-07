#include "dusk/file_cache.hpp"

#include "JSystem/JKernel/JKRDvdFile.h"
#include "dusk/settings.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <list>
#include <new>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dusk::file_cache {
namespace {

// 256 MiB is about 4% of Quest 2's 6 GB total RAM, leaving the rest for the OS and game heap.
constexpr std::size_t kMaxCacheBytes = 256u * 1024u * 1024u;
constexpr int kMaxCacheMB = static_cast<int>(kMaxCacheBytes / (1024u * 1024u));
using Key = std::uint64_t;

Key make_key(s32 entryNumber, s32 fileSize) {
    return (static_cast<Key>(static_cast<u32>(entryNumber)) << 32) | static_cast<u32>(fileSize);
}

std::size_t configured_capacity() {
    const int mb = std::clamp(getSettings().game.fileCacheMB.getValue(), 0, kMaxCacheMB);
    return static_cast<std::size_t>(mb) * 1024u * 1024u;
}

class Cache {
public:
    explicit Cache(std::size_t capacity = 0) : mCapacity(capacity) {}

    void set_capacity(std::size_t capacity) {
        std::lock_guard lock(mMutex);
        if (capacity != mCapacity) {
            mCapacity = capacity;
            mCaptures.clear();
            mReservedBytes = 0;
        }
        while (mBytes > mCapacity) {
            evict_one();
        }
    }

    bool begin_capture(Key key, std::size_t fileSize) {
        std::lock_guard lock(mMutex);
        if (fileSize == 0 || fileSize > mCapacity || mEntries.contains(key)) {
            return false;
        }
        if (auto capture = mCaptures.find(key); capture != mCaptures.end()) {
            ++capture->second.active;
            return true;
        }

        while (mBytes + mReservedBytes + fileSize > mCapacity && !mLru.empty()) {
            evict_one();
        }
        if (mBytes + mReservedBytes + fileSize > mCapacity) {
            return false;
        }
        try {
            auto [capture, inserted] = mCaptures.try_emplace(key, CaptureBuffer{fileSize, 1});
            if (!inserted) {
                ++capture->second.active;
                return true;
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        mReservedBytes += fileSize;
        return true;
    }

    void end_capture(Key key) {
        std::lock_guard lock(mMutex);
        auto capture = mCaptures.find(key);
        if (capture == mCaptures.end()) {
            return;
        }
        if (--capture->second.active == 0) {
            mReservedBytes -= capture->second.fileSize;
            mCaptures.erase(capture);
        }
    }

    bool read(Key key, void* buffer, s32 length, s32 offset, s32& result) {
        if (buffer == nullptr || length < 0 || offset < 0) {
            return false;
        }

        std::lock_guard lock(mMutex);
        auto entry = mEntries.find(key);
        if (entry == mEntries.end()) {
            return false;
        }
        const auto& bytes = entry->second.bytes;
        if (static_cast<std::size_t>(offset) > bytes.size()) {
            return false;
        }
        const std::size_t amount =
            std::min(static_cast<std::size_t>(length), bytes.size() - static_cast<std::size_t>(offset));
        if (amount != 0) {
            std::memcpy(buffer, bytes.data() + offset, amount);
        }
        result = static_cast<s32>(amount);
        mLru.splice(mLru.begin(), mLru, entry->second.lru);
        return true;
    }

    void record(Key key, std::size_t fileSize, const void* buffer, s32 length, s32 offset,
                s32 result) {
        if (buffer == nullptr || length <= 0 || offset < 0 || result <= 0 || fileSize == 0 ||
            static_cast<std::size_t>(offset) >= fileSize) {
            return;
        }

        const std::size_t amount = std::min(
            {static_cast<std::size_t>(length), static_cast<std::size_t>(result),
             fileSize - static_cast<std::size_t>(offset)});
        if (amount == 0) {
            return;
        }

        std::lock_guard lock(mMutex);
        if (fileSize > mCapacity) {
            return;
        }
        if (static_cast<std::size_t>(offset) == 0 && amount == fileSize) {
            if (auto pending = mCaptures.find(key); pending != mCaptures.end()) {
                mReservedBytes -= pending->second.fileSize;
                mCaptures.erase(pending);
            }
            store_copy(key, static_cast<const u8*>(buffer), fileSize);
            return;
        }

        auto capture = mCaptures.find(key);
        if (capture == mCaptures.end() || capture->second.failed) {
            return;
        }
        CaptureBuffer& pending = capture->second;
        if (pending.bytes.empty()) {
            try {
                pending.bytes.resize(fileSize);
            } catch (const std::bad_alloc&) {
                pending.failed = true;
                return;
            }
        }

        const std::size_t start = static_cast<std::size_t>(offset);
        const std::size_t end = start + amount;
        std::memcpy(pending.bytes.data() + start, buffer, amount);
        auto range = std::lower_bound(pending.ranges.begin(), pending.ranges.end(), start,
                                      [](const auto& item, std::size_t value) {
                                          return item.second < value;
                                      });
        std::size_t mergedStart = start;
        std::size_t mergedEnd = end;
        while (range != pending.ranges.end() && range->first <= mergedEnd) {
            mergedStart = std::min(mergedStart, range->first);
            mergedEnd = std::max(mergedEnd, range->second);
            pending.covered -= range->second - range->first;
            range = pending.ranges.erase(range);
        }
        try {
            range = pending.ranges.insert(range, {mergedStart, mergedEnd});
        } catch (const std::bad_alloc&) {
            pending.failed = true;
            return;
        }
        pending.covered += mergedEnd - mergedStart;

        if (pending.covered == fileSize) {
            std::vector<u8> completed = std::move(pending.bytes);
            mReservedBytes -= pending.fileSize;
            mCaptures.erase(capture);
            store_move(key, std::move(completed));
        }
    }

    std::size_t bytes_used() const {
        std::lock_guard lock(mMutex);
        return mBytes;
    }

    bool store_for_check(Key key, const u8* data, std::size_t size) {
        std::lock_guard lock(mMutex);
        return store_copy(key, data, size);
    }

private:
    struct Entry {
        std::vector<u8> bytes;
        std::list<Key>::iterator lru;
    };

    struct CaptureBuffer {
        std::size_t fileSize;
        std::size_t active;
        std::vector<u8> bytes;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::size_t covered = 0;
        bool failed = false;
    };

    bool store_copy(Key key, const u8* data, std::size_t size) {
        if (data == nullptr || size == 0 || size > mCapacity) {
            return false;
        }
        try {
            std::vector<u8> copy(data, data + size);
            store_move(key, std::move(copy));
            return mEntries.contains(key);
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void store_move(Key key, std::vector<u8>&& bytes) {
        if (bytes.empty() || bytes.size() > mCapacity) {
            return;
        }
        if (auto entry = mEntries.find(key); entry != mEntries.end()) {
            mLru.splice(mLru.begin(), mLru, entry->second.lru);
            return;
        }
        while (mBytes + mReservedBytes + bytes.size() > mCapacity && !mLru.empty()) {
            evict_one();
        }
        if (mBytes + mReservedBytes + bytes.size() > mCapacity) {
            return;
        }
        try {
            mLru.push_front(key);
            auto [entry, inserted] = mEntries.emplace(key, Entry{std::move(bytes), mLru.begin()});
            if (!inserted) {
                mLru.pop_front();
                mLru.splice(mLru.begin(), mLru, entry->second.lru);
                return;
            }
            mBytes += entry->second.bytes.size();
        } catch (const std::bad_alloc&) {
            if (!mLru.empty() && mLru.front() == key && !mEntries.contains(key)) {
                mLru.pop_front();
            }
        }
    }

    void evict_one() {
        const Key key = mLru.back();
        auto entry = mEntries.find(key);
        if (entry != mEntries.end()) {
            mBytes -= entry->second.bytes.size();
            mEntries.erase(entry);
        }
        mLru.pop_back();
    }

    mutable std::mutex mMutex;
    std::size_t mCapacity;
    std::size_t mBytes = 0;
    std::size_t mReservedBytes = 0;
    std::list<Key> mLru;
    std::unordered_map<Key, Entry> mEntries;
    std::unordered_map<Key, CaptureBuffer> mCaptures;
};

#ifndef NDEBUG
void run_self_check() {
    Cache cache(8);
    const std::array<u8, 4> first{1, 2, 3, 4};
    const std::array<u8, 4> second{5, 6, 7, 8};
    const std::array<u8, 4> third{9, 10, 11, 12};
    assert(cache.store_for_check(1, first.data(), first.size()));
    assert(cache.store_for_check(2, second.data(), second.size()));

    std::array<u8, 4> partial{99, 99, 99, 99};
    s32 result = -1;
    assert(cache.read(1, partial.data() + 1, 2, 1, result));
    assert(result == 2);
    assert(partial[0] == 99 && partial[1] == 2 && partial[2] == 3 && partial[3] == 99);

    assert(cache.store_for_check(3, third.data(), third.size()));
    assert(!cache.read(2, partial.data(), 4, 0, result));
    assert(cache.read(1, partial.data(), 4, 0, result));
    assert(cache.read(3, partial.data(), 4, 0, result));
    cache.set_capacity(4);
    assert(cache.bytes_used() == 4);
    assert(!cache.read(1, partial.data(), 4, 0, result));
    assert(cache.read(3, partial.data(), 4, 0, result));
}
#endif

Cache& cache() {
    static Cache instance;
#ifndef NDEBUG
    static const bool checked = [] {
        run_self_check();
        return true;
    }();
    (void)checked;
#endif
    return instance;
}

bool cache_key(JKRDvdFile* file, Key& key, std::size_t& fileSize) {
    if (file == nullptr) {
        return false;
    }
    const s32 entryNumber = file->getEntryNumber();
    const s32 size = file->getFileSize();
    if (entryNumber < 0 || size <= 0) {
        return false;
    }
    fileSize = static_cast<std::size_t>(size);
    key = make_key(entryNumber, size);
    return true;
}

} // namespace

Capture::Capture(JKRDvdFile* file, bool enabled) {
    Key key;
    std::size_t fileSize;
    if (!enabled || !cache_key(file, key, fileSize)) {
        return;
    }
    Cache& shared = cache();
    shared.set_capacity(configured_capacity());
    mActive = shared.begin_capture(key, fileSize);
    if (mActive) {
        mKey = key;
    }
}

Capture::~Capture() {
    if (mActive) {
        cache().end_capture(mKey);
    }
}

bool try_read(JKRDvdFile* file, void* buffer, s32 length, s32 offset, s32& result) {
    Key key;
    std::size_t fileSize;
    if (!cache_key(file, key, fileSize) || fileSize > kMaxCacheBytes) {
        return false;
    }
    Cache& shared = cache();
    const std::size_t capacity = configured_capacity();
    shared.set_capacity(capacity);
    return capacity != 0 && shared.read(key, buffer, length, offset, result);
}

void record_read(JKRDvdFile* file, const void* buffer, s32 length, s32 offset, s32 result) {
    Key key;
    std::size_t fileSize;
    if (!cache_key(file, key, fileSize) || fileSize > kMaxCacheBytes) {
        return;
    }
    Cache& shared = cache();
    const std::size_t capacity = configured_capacity();
    shared.set_capacity(capacity);
    if (capacity != 0) {
        shared.record(key, fileSize, buffer, length, offset, result);
    }
}

s32 read_prio(JKRDvdFile* file, void* buffer, s32 length, s32 offset, s32 priority) {
    s32 result;
    if (try_read(file, buffer, length, offset, result)) {
        return result;
    }
    result = DVDReadPrio(file->getFileInfo(), buffer, length, offset, priority);
    if (result >= 0) {
        record_read(file, buffer, length, offset, result);
    }
    return result;
}

} // namespace dusk::file_cache
