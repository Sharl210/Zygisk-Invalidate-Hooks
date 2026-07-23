#include "mainCore.hxx"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "log.h"
#include "xdl.h"

namespace {

constexpr std::size_t kMaxExecutableSegments = 64U;
constexpr std::size_t kMaxExecutableSegmentBytes = 512U * 1024U * 1024U;

struct ScopedFd {
    explicit ScopedFd(int value = -1) : value(value) {}
    ~ScopedFd() {
        if (value >= 0) {
            close(value);
        }
    }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    int value;
};

struct ScopedXdlHandle {
    explicit ScopedXdlHandle(void *value = nullptr) : value(value) {}
    ~ScopedXdlHandle() {
        if (value != nullptr) {
            xdl_close(value);
        }
    }
    ScopedXdlHandle(const ScopedXdlHandle &) = delete;
    ScopedXdlHandle &operator=(const ScopedXdlHandle &) = delete;

    void *value;
};

struct LoadSegment {
    uintptr_t address = 0;
    off_t fileOffset = 0;
    std::size_t size = 0;
};

struct RestoreRange {
    std::size_t segmentIndex = 0;
    uintptr_t address = 0;
    uintptr_t pageStart = 0;
    std::size_t pageLength = 0;
    int originalProtection = PROT_NONE;
    std::vector<unsigned char> expected;
    std::vector<unsigned char> original;
};

struct RestoreWriteResult {
    bool writeAttempted = false;
    bool memoryMayHaveChanged = false;
    bool usedRwxWrite = false;
    bool byteVerified = false;
    bool protectionRestored = false;
};

bool SafeAdd(uintptr_t left, std::size_t right, uintptr_t *result) {
    if (right > std::numeric_limits<uintptr_t>::max() - left) {
        return false;
    }
    *result = left + static_cast<uintptr_t>(right);
    return true;
}

std::string HexPreview(const std::vector<unsigned char> &bytes, std::size_t offset,
                       std::size_t maximumBytes = 16U) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (offset >= bytes.size()) {
        return {};
    }
    const std::size_t count = std::min(maximumBytes, bytes.size() - offset);
    std::string result;
    result.reserve(count * 3U + 3U);
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char value = bytes[offset + index];
        if (index != 0) {
            result.push_back(' ');
        }
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    if (offset + count < bytes.size()) {
        result += " ...";
    }
    return result;
}

bool ReadExactAt(int fd, void *buffer, std::size_t size, off_t offset, std::string *error) {
    unsigned char *destination = static_cast<unsigned char *>(buffer);
    std::size_t completed = 0;
    while (completed < size) {
        if (completed > static_cast<std::size_t>(std::numeric_limits<off_t>::max() - offset)) {
            *error = "file offset overflow while reading executable segment";
            return false;
        }
        const ssize_t count = pread(fd, destination + completed, size - completed,
                                    offset + static_cast<off_t>(completed));
        if (count == 0) {
            *error = "unexpected end of library file";
            return false;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("pread failed: ") + std::strerror(errno);
            return false;
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

bool BuildExecutableSegments(const xdl_info_t &info, const struct stat &fileStat,
                             std::vector<LoadSegment> *segments, std::string *error) {
    if (info.dli_fbase == nullptr || info.dlpi_phdr == nullptr || info.dlpi_phnum == 0) {
        *error = "xDL returned incomplete loaded-library metadata";
        return false;
    }
    if (fileStat.st_size < 0) {
        *error = "library file has an invalid size";
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(fileStat.st_size);

    for (std::size_t index = 0; index < info.dlpi_phnum; ++index) {
        const ElfW(Phdr) &header = info.dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0 || header.p_filesz == 0) {
            continue;
        }
        if (header.p_memsz < header.p_filesz) {
            *error = "executable ELF segment has p_memsz smaller than p_filesz";
            return false;
        }

        const std::uint64_t fileOffset = static_cast<std::uint64_t>(header.p_offset);
        const std::uint64_t fileBytes = static_cast<std::uint64_t>(header.p_filesz);
        if (fileOffset > fileSize || fileBytes > fileSize - fileOffset) {
            *error = "executable ELF segment exceeds the on-disk library";
            return false;
        }
        if (fileBytes == 0 || fileBytes > kMaxExecutableSegmentBytes ||
            fileBytes > std::numeric_limits<std::size_t>::max() ||
            fileOffset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            *error = "executable ELF segment exceeds V2 safety bounds";
            return false;
        }
        if (header.p_vaddr > std::numeric_limits<uintptr_t>::max() - base) {
            *error = "loaded-library base address overflow";
            return false;
        }

        const uintptr_t address = base + static_cast<uintptr_t>(header.p_vaddr);
        uintptr_t end = 0;
        const std::size_t size = static_cast<std::size_t>(fileBytes);
        if (!SafeAdd(address, size, &end)) {
            *error = "executable segment address range overflow";
            return false;
        }
        (void) end;

        if (segments->size() >= kMaxExecutableSegments) {
            *error = "too many executable ELF segments";
            return false;
        }
        segments->push_back({address, static_cast<off_t>(fileOffset), size});
    }

    if (segments->empty()) {
        *error = "no executable PT_LOAD segment was found";
        return false;
    }
    return true;
}

bool PageSpanForRange(uintptr_t address, std::size_t size, uintptr_t *pageStart,
                      std::size_t *pageLength, std::string *error) {
    if (size == 0) {
        *error = "zero-length restore range";
        return false;
    }
    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        *error = "could not determine system page size";
        return false;
    }
    const uintptr_t pageSize = static_cast<uintptr_t>(rawPageSize);
    if ((pageSize & (pageSize - 1U)) != 0U) {
        *error = "system page size is not a power of two";
        return false;
    }

    uintptr_t rangeEnd = 0;
    if (!SafeAdd(address, size, &rangeEnd)) {
        *error = "restore range address overflow";
        return false;
    }
    const uintptr_t start = address & ~(pageSize - 1U);
    if (rangeEnd > std::numeric_limits<uintptr_t>::max() - (pageSize - 1U)) {
        *error = "restore page alignment overflow";
        return false;
    }
    const uintptr_t end = (rangeEnd + pageSize - 1U) & ~(pageSize - 1U);
    if (end <= start || end - start > std::numeric_limits<std::size_t>::max()) {
        *error = "invalid restore page span";
        return false;
    }

    *pageStart = start;
    *pageLength = static_cast<std::size_t>(end - start);
    return true;
}

bool FindOriginalProtection(uintptr_t pageStart, std::size_t pageLength, int *protection,
                            std::string *error) {
    uintptr_t pageEnd = 0;
    if (!SafeAdd(pageStart, pageLength, &pageEnd)) {
        *error = "maps lookup range overflow";
        return false;
    }

    FILE *maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        *error = std::string("could not open /proc/self/maps: ") + std::strerror(errno);
        return false;
    }

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long mappingStart = 0;
        unsigned long long mappingEnd = 0;
        char permissions[5] = {};
        if (std::sscanf(line, "%llx-%llx %4s", &mappingStart, &mappingEnd, permissions) != 3) {
            continue;
        }
        if (mappingEnd <= mappingStart || mappingStart > std::numeric_limits<uintptr_t>::max() ||
            mappingEnd > std::numeric_limits<uintptr_t>::max()) {
            continue;
        }
        const uintptr_t start = static_cast<uintptr_t>(mappingStart);
        const uintptr_t end = static_cast<uintptr_t>(mappingEnd);
        if (pageStart < start || pageEnd > end) {
            continue;
        }

        // Preserve original behavior for any executable mapping. The actual
        // mprotect/write/restore sequence below remains the final authority.
#if defined(INLINE_HOOK_SPOOF_HOST_TEST)
        const bool mappingAccepted = permissions[0] == 'r';
#else
        const bool mappingAccepted = permissions[2] == 'x';
#endif
        if (!mappingAccepted) {
            std::fclose(maps);
            *error = "target pages are not an accepted executable mapping";
            return false;
        }
        int result = PROT_NONE;
        if (permissions[0] == 'r') {
            result |= PROT_READ;
        }
        if (permissions[1] == 'w') {
            result |= PROT_WRITE;
        }
        if (permissions[2] == 'x') {
            result |= PROT_EXEC;
        }
        std::fclose(maps);
        *protection = result;
        return true;
    }

    std::fclose(maps);
    *error = "target pages are not covered by one verified /proc/self/maps entry";
    return false;
}


bool AddSegmentRestore(const LoadSegment &segment, std::size_t segmentIndex,
                       const std::vector<unsigned char> &diskBytes,
                       const std::vector<unsigned char> &memoryBytes, bool captureArtifactSnapshots,
                       RestoreStats *stats, std::vector<RestoreRange> *plan, std::string *error) {
    if (diskBytes.size() != memoryBytes.size() || diskBytes.empty()) {
        *error = "invalid executable segment snapshot";
        return false;
    }

    RestoreRange range;
    range.segmentIndex = segmentIndex;
    range.address = segment.address;
    range.expected = diskBytes;
    range.original = memoryBytes;
    if (!PageSpanForRange(range.address, range.expected.size(), &range.pageStart, &range.pageLength, error) ||
        !FindOriginalProtection(range.pageStart, range.pageLength, &range.originalProtection, error)) {
        return false;
    }

    RestoreSegmentRecord record;
    record.segmentIndex = segmentIndex;
    record.memoryAddress = segment.address;
    record.fileOffset = static_cast<std::size_t>(segment.fileOffset);
    record.size = segment.size;
    record.pageStart = range.pageStart;
    record.pageLength = range.pageLength;
    record.originalProtection = range.originalProtection;

    bool inDifferenceRange = false;
    std::size_t differenceRangeStart = 0;
    for (std::size_t index = 0; index < diskBytes.size(); ++index) {
        if (diskBytes[index] == memoryBytes[index]) {
            if (inDifferenceRange) {
                record.differenceRanges.push_back({differenceRangeStart, index - differenceRangeStart});
                inDifferenceRange = false;
            }
            continue;
        }
        if (!record.hasDifference) {
            record.hasDifference = true;
            record.firstDifferenceOffset = index;
            record.diskBytesAtFirstDifference = HexPreview(diskBytes, index);
            record.memoryBytesAtFirstDifference = HexPreview(memoryBytes, index);
        }
        if (!inDifferenceRange) {
            differenceRangeStart = index;
            inDifferenceRange = true;
        }
        record.lastDifferenceOffset = index;
        ++record.differingBytes;
    }
    if (inDifferenceRange) {
        record.differenceRanges.push_back({differenceRangeStart, diskBytes.size() - differenceRangeStart});
    }
    if (captureArtifactSnapshots && record.hasDifference) {
        record.artifactBeforeBytes = memoryBytes;
        record.artifactAfterBytes = diskBytes;
    }
    stats->differingBytes += record.differingBytes;
    if (record.hasDifference) {
        ++stats->differingRanges;
    }
    stats->segments.push_back(std::move(record));
    plan->push_back(std::move(range));
    return true;
}

bool BuildRestorePlan(int fd, const std::vector<LoadSegment> &segments, bool captureArtifactSnapshots,
                      RestoreStats *stats, std::vector<RestoreRange> *plan, std::string *error) {
    // Compatibility contract: each configured executable PT_LOAD segment is
    // restored as a complete segment, matching the original module behavior.
    // The disk snapshot and original bytes are retained only for verification
    // and rollback, not to impose a difference-size policy.
    for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const LoadSegment &segment = segments[segmentIndex];
        ++stats->executableSegments;
        std::vector<unsigned char> diskBytes(segment.size);
        if (!ReadExactAt(fd, diskBytes.data(), diskBytes.size(), segment.fileOffset, error)) {
            return false;
        }

        std::vector<unsigned char> memoryBytes(segment.size);
        std::memcpy(memoryBytes.data(), reinterpret_cast<const void *>(segment.address), segment.size);
        if (!AddSegmentRestore(segment, segmentIndex, diskBytes, memoryBytes, captureArtifactSnapshots,
                               stats, plan, error)) {
            return false;
        }
    }
    return true;
}

bool MakeRangeWritableExecutable(const RestoreRange &range, bool *usedRwxWrite, std::string *error) {
    // Preserve the original module's critical execution invariant: code pages
    // remain executable while their bytes are restored. Dropping PROT_EXEC to
    // prefer RW can race ART/LSP execution and cause SEGV_ACCERR.
    *usedRwxWrite = false;
#if defined(INLINE_HOOK_SPOOF_HOST_TEST)
    // The host test sandbox rejects executable anonymous mappings. This branch
    // exists only for fixture validation; Android production always follows
    // the RWX path below.
    if (mprotect(reinterpret_cast<void *>(range.pageStart), range.pageLength, PROT_READ | PROT_WRITE) == 0) {
        *usedRwxWrite = true;
        return true;
    }
    *error = std::string("host fixture mprotect(RW) failed: ") + std::strerror(errno);
    return false;
#else
    if (mprotect(reinterpret_cast<void *>(range.pageStart), range.pageLength,
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        *usedRwxWrite = true;
        return true;
    }
    *error = std::string("mprotect(RWX) failed: ") + std::strerror(errno);
    return false;
#endif
}

bool RestoreOneRange(const RestoreRange &range, RestoreWriteResult *result, std::string *error) {
    *result = {};
    result->writeAttempted = true;
    if (!MakeRangeWritableExecutable(range, &result->usedRwxWrite, error)) {
        return false;
    }

    std::memcpy(reinterpret_cast<void *>(range.address), range.expected.data(), range.expected.size());
    result->memoryMayHaveChanged = true;
    __builtin___clear_cache(reinterpret_cast<char *>(range.address),
                            reinterpret_cast<char *>(range.address + range.expected.size()));
    result->byteVerified = std::memcmp(reinterpret_cast<const void *>(range.address), range.expected.data(),
                                       range.expected.size()) == 0;
    result->protectionRestored =
        mprotect(reinterpret_cast<void *>(range.pageStart), range.pageLength, range.originalProtection) == 0;

    if (!result->byteVerified || !result->protectionRestored) {
        if (!result->byteVerified && !result->protectionRestored) {
            *error = "post-write verification and protection restoration both failed";
        } else if (!result->byteVerified) {
            *error = "post-write byte verification failed";
        } else {
            *error = std::string("could not restore original page protection: ") + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool RollBackOneRange(const RestoreRange &range, std::string *error) {
    bool ignoredRwxWrite = false;
    if (!MakeRangeWritableExecutable(range, &ignoredRwxWrite, error)) {
        *error = "rollback " + *error;
        return false;
    }

    std::memcpy(reinterpret_cast<void *>(range.address), range.original.data(), range.original.size());
    __builtin___clear_cache(reinterpret_cast<char *>(range.address),
                            reinterpret_cast<char *>(range.address + range.original.size()));
    const bool verified = std::memcmp(reinterpret_cast<const void *>(range.address), range.original.data(),
                                      range.original.size()) == 0;
    const bool protectionRestored =
        mprotect(reinterpret_cast<void *>(range.pageStart), range.pageLength, range.originalProtection) == 0;
    if (!verified || !protectionRestored) {
        *error = "rollback verification or protection restoration failed";
        return false;
    }
    return true;
}

bool RollBackWrittenRanges(const std::vector<RestoreRange> &plan,
                           const std::vector<std::size_t> &writtenIndices, std::string *error) {
    bool allSucceeded = true;
    std::string firstError;
    for (auto iterator = writtenIndices.rbegin(); iterator != writtenIndices.rend(); ++iterator) {
        std::string rollbackError;
        if (!RollBackOneRange(plan[*iterator], &rollbackError)) {
            allSucceeded = false;
            if (firstError.empty()) {
                firstError = rollbackError;
            }
        }
    }
    if (!allSucceeded) {
        *error = firstError;
    }
    return allSucceeded;
}

}  // namespace

const char *RestoreModeName(RestoreMode mode) {
    return mode == RestoreMode::kRestore ? "restore" : "audit";
}

const char *RestoreOutcomeName(RestoreOutcome outcome) {
    switch (outcome) {
        case RestoreOutcome::kSkipped:
            return "skipped";
        case RestoreOutcome::kAlreadyClean:
            return "already_clean";
        case RestoreOutcome::kAudited:
            return "audited";
        case RestoreOutcome::kRestored:
            return "restored";
        case RestoreOutcome::kFailed:
            return "failed";
    }
    return "unknown";
}

RestoreStats InspectOrRestoreLibrary(const RestoreConfig &config) {
    RestoreStats stats;
    stats.requestedLibrary = config.libraryName;
    if (!config.enabled) {
        stats.detail = "configuration disabled";
        return stats;
    }
    if (config.libraryName.empty()) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = "configured library name is empty";
        return stats;
    }
    ScopedXdlHandle handle(xdl_open(config.libraryName.c_str(), XDL_DEFAULT));
    if (handle.value == nullptr) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = "could not resolve configured loaded library";
        return stats;
    }

    xdl_info_t info{};
    if (xdl_info(handle.value, XDL_DI_DLINFO, &info) != 0 || info.dli_fname == nullptr ||
        info.dli_fname[0] == '\0') {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = "could not obtain configured library metadata";
        return stats;
    }

    stats.resolvedLibraryPath = info.dli_fname;
    stats.libraryBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
    stats.programHeaderCount = info.dlpi_phnum;

    ScopedFd fd(open(info.dli_fname, O_RDONLY | O_CLOEXEC));
    if (fd.value < 0) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = std::string("could not open mapped library path: ") + std::strerror(errno);
        return stats;
    }

    struct stat fileStat {};
    if (fstat(fd.value, &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = "mapped library path is not a regular readable file";
        return stats;
    }

    stats.libraryFileSize = static_cast<std::uint64_t>(fileStat.st_size);
    stats.libraryDevice = static_cast<std::uint64_t>(fileStat.st_dev);
    stats.libraryInode = static_cast<std::uint64_t>(fileStat.st_ino);

    std::vector<LoadSegment> segments;
    std::string error;
    if (!BuildExecutableSegments(info, fileStat, &segments, &error)) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = error;
        return stats;
    }

    std::vector<RestoreRange> plan;
    if (!BuildRestorePlan(fd.value, segments, config.captureArtifactSnapshots,
                          &stats, &plan, &error)) {
        stats.outcome = RestoreOutcome::kFailed;
        stats.detail = error;
        return stats;
    }
    if (config.mode == RestoreMode::kAudit) {
        stats.outcome = RestoreOutcome::kAudited;
        stats.detail = "difference recorded; audit mode never writes memory";
        return stats;
    }

    std::vector<std::size_t> writtenIndices;
    writtenIndices.reserve(plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
        RestoreSegmentRecord &record = stats.segments[plan[index].segmentIndex];
        RestoreWriteResult writeResult;
        if (!RestoreOneRange(plan[index], &writeResult, &error)) {
            record.writeAttempted = writeResult.writeAttempted;
            record.usedRwxWrite = writeResult.usedRwxWrite;
            record.byteVerified = writeResult.byteVerified;
            record.protectionRestored = writeResult.protectionRestored;
            record.failure = error;
            if (writeResult.memoryMayHaveChanged) {
                writtenIndices.push_back(index);
            }
            if (!writtenIndices.empty()) {
                stats.rollbackAttempted = true;
                std::string rollbackError;
                stats.rollbackSucceeded = RollBackWrittenRanges(plan, writtenIndices, &rollbackError);
                for (const std::size_t writtenIndex : writtenIndices) {
                    RestoreSegmentRecord &writtenRecord = stats.segments[plan[writtenIndex].segmentIndex];
                    writtenRecord.rollbackAttempted = true;
                    writtenRecord.rollbackSucceeded = stats.rollbackSucceeded;
                    if (stats.rollbackSucceeded) {
                        writtenRecord.restored = false;
                    }
                }
                if (!stats.rollbackSucceeded) {
                    error += "; rollback failed: " + rollbackError;
                }
            }
            stats.outcome = RestoreOutcome::kFailed;
            stats.detail = error;
            return stats;
        }
        record.writeAttempted = writeResult.writeAttempted;
        record.usedRwxWrite = writeResult.usedRwxWrite;
        record.byteVerified = writeResult.byteVerified;
        record.protectionRestored = writeResult.protectionRestored;
        record.restored = true;
        writtenIndices.push_back(index);
        stats.restoredBytes += plan[index].expected.size();
    }

    stats.outcome = RestoreOutcome::kRestored;
    stats.detail = "all configured executable segments were restored and verified";
    return stats;
}
