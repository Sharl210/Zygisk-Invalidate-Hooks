#ifndef ZYGISK_INVALIDATE_HOOKS_MAIN_CORE_HXX
#define ZYGISK_INVALIDATE_HOOKS_MAIN_CORE_HXX

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One restore invocation for one configured library in one selected process.
enum class RestoreMode {
    kAudit,
    kRestore,
};

enum class RestoreOutcome {
    kSkipped,
    kAlreadyClean,
    kAudited,
    kRestored,
    kFailed,
};

struct RestoreConfig {
    bool enabled = true;
    RestoreMode mode = RestoreMode::kRestore;
    // Only enabled for selected rules with detailed logging. Snapshots are
    // retained after a real difference is observed so the root companion can
    // materialize before/after SO artifacts after restoration completes.
    bool captureArtifactSnapshots = false;
    std::string libraryName = "libart.so";
};

struct RestoreDifferenceRange {
    std::size_t offset = 0;
    std::size_t length = 0;
};

// A per-executable-PT_LOAD evidence record. These records are held only for the
// current process and emitted only when the application's log switch is enabled.
struct RestoreSegmentRecord {
    std::size_t segmentIndex = 0;
    uintptr_t memoryAddress = 0;
    std::size_t fileOffset = 0;
    std::size_t size = 0;
    uintptr_t pageStart = 0;
    std::size_t pageLength = 0;
    int originalProtection = 0;
    bool hasDifference = false;
    std::size_t differingBytes = 0;
    std::size_t firstDifferenceOffset = 0;
    std::size_t lastDifferenceOffset = 0;
    std::string diskBytesAtFirstDifference;
    std::string memoryBytesAtFirstDifference;
    std::vector<RestoreDifferenceRange> differenceRanges;
    // Full executable-segment snapshots used only by the artifact writer.
    std::vector<unsigned char> artifactBeforeBytes;
    std::vector<unsigned char> artifactAfterBytes;
    bool writeAttempted = false;
    bool usedRwxWrite = false;
    bool byteVerified = false;
    bool protectionRestored = false;
    bool restored = false;
    bool rollbackAttempted = false;
    bool rollbackSucceeded = false;
    std::string failure;
};

struct RestoreStats {
    RestoreOutcome outcome = RestoreOutcome::kSkipped;
    std::string requestedLibrary;
    std::string resolvedLibraryPath;
    uintptr_t libraryBase = 0;
    std::size_t programHeaderCount = 0;
    std::uint64_t libraryFileSize = 0;
    std::uint64_t libraryDevice = 0;
    std::uint64_t libraryInode = 0;
    std::size_t executableSegments = 0;
    std::size_t differingRanges = 0;
    std::size_t differingBytes = 0;
    std::size_t restoredBytes = 0;
    bool rollbackAttempted = false;
    bool rollbackSucceeded = false;
    std::string detail;
    std::vector<RestoreSegmentRecord> segments;
};

const char *RestoreModeName(RestoreMode mode);
const char *RestoreOutcomeName(RestoreOutcome outcome);

// Restore the complete executable PT_LOAD segments of one already-loaded,
// configured library. This preserves the original module's behavior while V2
// validates reads/ranges and verifies writes and rollback attempts.
RestoreStats InspectOrRestoreLibrary(const RestoreConfig &config);

#endif  // ZYGISK_INVALIDATE_HOOKS_MAIN_CORE_HXX
