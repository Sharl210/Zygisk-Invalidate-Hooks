#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <link.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "mainCore.hxx"
#include "xdl.h"

namespace {

xdl_info_t gInfo{};
ElfW(Phdr) gProgramHeader{};

[[noreturn]] void Fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

void Require(bool condition, const char *message) {
    if (!condition) {
        Fail(message);
    }
}

void WriteAll(int fd, const std::vector<unsigned char> &bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + done, bytes.size() - done);
        if (written <= 0) {
            Fail("could not populate fixture library");
        }
        done += static_cast<std::size_t>(written);
    }
}

void Mutate(void *mapping, std::size_t pageSize, std::size_t offset, std::size_t count) {
    (void) pageSize;
    const uintptr_t address = reinterpret_cast<uintptr_t>(mapping) + offset;
    std::vector<unsigned char> bytes(count);
    std::memcpy(bytes.data(), reinterpret_cast<const void *>(address), count);
    for (unsigned char &byte : bytes) {
        byte ^= 0xA5U;
    }

    const int memoryFd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    Require(memoryFd >= 0, "could not open /proc/self/mem for test fixture");
    const ssize_t written = pwrite(memoryFd, bytes.data(), bytes.size(), static_cast<off_t>(address));
    close(memoryFd);
    Require(written == static_cast<ssize_t>(bytes.size()), "could not mutate RX fixture through /proc/self/mem");
    __builtin___clear_cache(reinterpret_cast<char *>(mapping), reinterpret_cast<char *>(mapping) + count);
}

bool Matches(const void *mapping, const std::vector<unsigned char> &expected) {
    return std::memcmp(mapping, expected.data(), expected.size()) == 0;
}

}  // namespace

extern "C" void *xdl_open(const char *, int) {
    return reinterpret_cast<void *>(0x1);
}

extern "C" void *xdl_close(void *) {
    return nullptr;
}

extern "C" int xdl_info(void *, int request, void *out) {
    if (request != XDL_DI_DLINFO || out == nullptr) {
        return -1;
    }
    *static_cast<xdl_info_t *>(out) = gInfo;
    return 0;
}

int main() {
    const long rawPageSize = sysconf(_SC_PAGESIZE);
    Require(rawPageSize > 0, "invalid host page size");
    const std::size_t pageSize = static_cast<std::size_t>(rawPageSize);

    char path[] = "/tmp/inline-hook-spoof-v2-test-XXXXXX";
    const int fd = mkstemp(path);
    Require(fd >= 0, "mkstemp failed");

    std::vector<unsigned char> clean(pageSize);
    for (std::size_t i = 0; i < clean.size(); ++i) {
        clean[i] = static_cast<unsigned char>((i * 17U + 31U) & 0xFFU);
    }
    WriteAll(fd, clean);
    Require(fsync(fd) == 0, "fixture fsync failed");

#if defined(INLINE_HOOK_SPOOF_HOST_TEST)
    const int fixtureProtection = PROT_READ;
#else
    const int fixtureProtection = PROT_READ | PROT_EXEC;
#endif
    void *mapping = mmap(nullptr, pageSize, fixtureProtection, MAP_PRIVATE, fd, 0);
    Require(mapping != MAP_FAILED, "fixture mmap failed");

    std::memset(&gProgramHeader, 0, sizeof(gProgramHeader));
    gProgramHeader.p_type = PT_LOAD;
    gProgramHeader.p_flags = PF_R | PF_X;
    gProgramHeader.p_offset = 0;
    gProgramHeader.p_vaddr = 0;
    gProgramHeader.p_filesz = pageSize;
    gProgramHeader.p_memsz = pageSize;
    gInfo = {};
    gInfo.dli_fname = path;
    gInfo.dli_fbase = mapping;
    gInfo.dlpi_phdr = &gProgramHeader;
    gInfo.dlpi_phnum = 1;

    RestoreConfig config;
    config.enabled = true;
    config.libraryName = "libcustom_target.so";

    Mutate(mapping, pageSize, 37, 6);
    config.mode = RestoreMode::kAudit;
    const RestoreStats audited = InspectOrRestoreLibrary(config);
    Require(audited.outcome == RestoreOutcome::kAudited, "audit must report differences");
    Require(audited.differingBytes == 6, "audit must count exact differing bytes");
    Require(audited.segments.size() == 1 && audited.segments[0].hasDifference &&
                audited.segments[0].differingBytes == 6 &&
                audited.segments[0].firstDifferenceOffset == 37 &&
                audited.segments[0].lastDifferenceOffset == 42 &&
                !audited.segments[0].diskBytesAtFirstDifference.empty() &&
                !audited.segments[0].memoryBytesAtFirstDifference.empty(),
            "audit must retain segment-level detection evidence");
    Require(!Matches(mapping, clean), "audit must not modify memory");

    config.mode = RestoreMode::kRestore;
    config.captureArtifactSnapshots = true;
    const RestoreStats restored = InspectOrRestoreLibrary(config);
    if (restored.outcome != RestoreOutcome::kRestored) {
        std::fprintf(stderr, "restore outcome=%s detail=%s rollbackAttempted=%d rollbackSucceeded=%d\\n",
                     RestoreOutcomeName(restored.outcome), restored.detail.c_str(), restored.rollbackAttempted,
                     restored.rollbackSucceeded);
        Fail("restore must complete");
    }
    Require(restored.restoredBytes == pageSize, "compatibility restore must write the complete executable segment");
    Require(restored.segments.size() == 1 && restored.segments[0].writeAttempted &&
                restored.segments[0].usedRwxWrite && restored.segments[0].byteVerified && restored.segments[0].protectionRestored &&
                restored.segments[0].restored,
            "restore must retain write, verification, and protection evidence");
    Require(restored.segments[0].differenceRanges.size() == 1 &&
                restored.segments[0].differenceRanges[0].offset == 37 &&
                restored.segments[0].differenceRanges[0].length == 6 &&
                restored.segments[0].artifactBeforeBytes.size() == pageSize &&
                restored.segments[0].artifactAfterBytes == clean &&
                restored.segments[0].artifactBeforeBytes[37] != clean[37],
            "artifact snapshots must retain exact pre/post difference evidence");
    Require(Matches(mapping, clean), "restore must return memory to original file bytes");

    // A large changed region must remain compatible with the original module's
    // whole-segment behavior without any V2 difference-size rejection.
    Mutate(mapping, pageSize, 101, 1024);
    const RestoreStats compatibilityRestored = InspectOrRestoreLibrary(config);
    Require(compatibilityRestored.outcome == RestoreOutcome::kRestored,
            "large differences must not be rejected by a V2 byte budget");
    Require(compatibilityRestored.restoredBytes == pageSize,
            "compatibility restore must write the full executable segment");
    Require(Matches(mapping, clean), "large-difference restore must return memory to disk bytes");

    Require(munmap(mapping, pageSize) == 0, "fixture munmap failed");
    close(fd);
    unlink(path);
    std::puts("PASS: audit does not write; custom-library complete-segment restore works; large differences remain compatible");
    return 0;
}
