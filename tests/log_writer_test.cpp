#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <regex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../module/src/main/cpp/main.cpp"

RestoreStats InspectOrRestoreLibrary(const RestoreConfig &) {
    return {};
}
const char *RestoreModeName(RestoreMode) {
    return "restore";
}
const char *RestoreOutcomeName(RestoreOutcome outcome) {
    return outcome == RestoreOutcome::kRestored ? "restored" : "failed";
}

namespace {

[[noreturn]] void Fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

void Require(bool condition, const char *message) {
    if (!condition) {
        Fail(message);
    }
}

std::string ReadFile(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "re");
    Require(file != nullptr, "could not read created log file");
    std::string result;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        result += buffer;
    }
    std::fclose(file);
    return result;
}

}  // namespace

int main() {
    char rootTemplate[] = "/tmp/inline-hook-spoof-log-test-XXXXXX";
    char *root = mkdtemp(rootTemplate);
    Require(root != nullptr, "mkdtemp failed");
    const int rootFd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(rootFd >= 0, "could not open temporary module directory");

    AppRule rule;
    rule.packageName = "com.example.target";
    rule.uid = 11010U;
    rule.libraries = {"libart.so", "libtarget_extra.so"};
    rule.logEnabled = true;

    RestoreStats stats;
    stats.outcome = RestoreOutcome::kRestored;
    stats.requestedLibrary = "libart.so";
    stats.resolvedLibraryPath = "/apex/com.android.art/lib64/libart.so";
    stats.libraryBase = 0x70000000U;
    stats.programHeaderCount = 12;
    stats.libraryFileSize = 123456U;
    stats.libraryDevice = 42U;
    stats.libraryInode = 99U;
    stats.executableSegments = 1;
    stats.differingRanges = 1;
    stats.differingBytes = 8;
    stats.restoredBytes = 4096;
    stats.detail = "all configured executable segments were restored and verified";

    RestoreSegmentRecord segment;
    segment.segmentIndex = 0;
    segment.memoryAddress = 0x70001000U;
    segment.fileOffset = 0x2000U;
    segment.size = 4096;
    segment.pageStart = 0x70001000U;
    segment.pageLength = 4096;
    segment.originalProtection = PROT_READ | PROT_EXEC;
    segment.hasDifference = true;
    segment.differingBytes = 8;
    segment.firstDifferenceOffset = 0x10U;
    segment.lastDifferenceOffset = 0x17U;
    segment.diskBytesAtFirstDifference = "1F 20 03 D5";
    segment.memoryBytesAtFirstDifference = "00 00 00 14";
    segment.writeAttempted = true;
    segment.byteVerified = true;
    segment.protectionRestored = true;
    segment.restored = true;
    stats.segments.push_back(segment);

    std::string error;
    std::string fileName;
    std::string timestamp;
    Require(FormatLogTime(&fileName, &timestamp, &error), "could not format detailed log timestamp");
    const std::string artifactFields =
        "ARTIFACT_STATUS=created\n"
        "ARTIFACT_DIRECTORY=artifacts/com.example.target_11010/20260723-123456_libart.so\n";
    const std::string payload = BuildRestoreLogPayload(rule, rule.packageName,
                                                        "preAppSpecialize_package_uid_root_companion",
                                                        timestamp, stats, artifactFields);
    Require(WriteLogPayloadAt(rootFd, rule.packageName, rule.uid, fileName, payload, &error),
            "first detailed log write must succeed");
    Require(WriteLogPayloadAt(rootFd, rule.packageName, rule.uid, fileName, payload, &error),
            "second detailed log write must preserve/append history");
    close(rootFd);

    const std::string directory = std::string(root) + "/logs/com.example.target_11010";
    DIR *dir = opendir(directory.c_str());
    Require(dir != nullptr, "per-app log directory missing");
    std::string logName;
    while (const dirent *entry = readdir(dir)) {
        if (std::regex_match(entry->d_name, std::regex("[0-9]{8}-[0-9]{6}\\.log"))) {
            logName = entry->d_name;
            break;
        }
    }
    closedir(dir);
    Require(!logName.empty(), "timestamp log file missing");

    const std::string contents = ReadFile(directory + "/" + logName);
    for (const char *token : {
             "PACKAGE=com.example.target",
             "PROCESS=com.example.target",
             "RECOVERY_STAGE=preAppSpecialize_package_uid_root_companion",
             "RULE_LIBRARIES=libart.so,libtarget_extra.so",
             "RESOLVED_LIBRARY_PATH=/apex/com.android.art/lib64/libart.so",
             "SEGMENT_0_FILE_OFFSET=0x2000",
             "SEGMENT_0_FIRST_DIFF_OFFSET=0x10",
             "SEGMENT_0_DISK_BYTES_AT_FIRST_DIFF=1F 20 03 D5",
             "SEGMENT_0_MEMORY_BYTES_AT_FIRST_DIFF=00 00 00 14",
             "SEGMENT_0_BYTE_VERIFIED=true",
             "SEGMENT_0_PROTECTION_RESTORED=true",
             "ARTIFACT_STATUS=created",
             "ARTIFACT_DIRECTORY=artifacts/com.example.target_11010/20260723-123456_libart.so",
             "END_EVENT",
         }) {
        Require(contents.find(token) != std::string::npos, "detailed log field missing");
    }

    std::string cleanup = std::string("rm -rf '") + root + "'";
    const int cleanupResult = std::system(cleanup.c_str());
    Require(cleanupResult == 0, "temporary log cleanup failed");
    std::puts("PASS: per-app timestamp logs preserve detailed restoration evidence");
    return 0;
}
