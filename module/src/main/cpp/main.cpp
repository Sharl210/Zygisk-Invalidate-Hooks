#include <jni.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "log.h"
#include "mainCore.hxx"
#include "zygisk.hpp"

namespace {

constexpr const char *kConfigName = "config.txt";
constexpr std::size_t kMaxConfigBytes = 64U * 1024U;
constexpr const char *kDefaultLibrary = "libart.so";

struct AppRule {
    std::string packageName;
    std::vector<std::string> libraries;
    bool logEnabled = false;
    std::uint32_t uid = 0;
};

struct ModuleConfig {
    bool enabled = false;
    bool perAppFormat = false;
    std::vector<AppRule> rules;
};

std::string Trim(std::string value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

bool IsSafePackageName(const std::string &value) {
    if (value.empty() || value.size() > 255 || value.find('.') == std::string::npos) {
        return false;
    }
    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '.' || ch == '$')) {
            return false;
        }
    }
    return true;
}

bool IsSafeLibraryName(const std::string &value) {
    // Keep custom library names/paths available. A comma is reserved only as
    // the per-app list separator; control characters cannot be represented in
    // the line-oriented configuration format.
    if (value.empty() || value.size() > 4096 || value.find(',') != std::string::npos) {
        return false;
    }
    for (unsigned char ch : value) {
        if (std::iscntrl(ch)) {
            return false;
        }
    }
    return true;
}

bool ParseBool(const std::string &value, bool *out) {
    if (value == "true") {
        *out = true;
        return true;
    }
    if (value == "false") {
        *out = false;
        return true;
    }
    return false;
}

bool ParseUid(const std::string &value, std::uint32_t *out) {
    if (value.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed == 0U ||
        parsed > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ReadSmallFileAt(int dirFd, const char *name, std::string *content, std::string *error) {
    const int fd = openat(dirFd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            return false;
        }
        *error = std::string("open ") + name + " failed: " + std::strerror(errno);
        return false;
    }

    std::string result;
    result.reserve(4096);
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            *error = std::string("read ") + name + " failed: " + std::strerror(errno);
            return false;
        }
        if (result.size() + static_cast<std::size_t>(count) > kMaxConfigBytes) {
            close(fd);
            *error = std::string(name) + " exceeds the 64 KiB configuration limit";
            return false;
        }
        result.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    *content = std::move(result);
    return true;
}

std::string FirstMeaningfulLine(const std::string &content) {
    std::size_t cursor = 0;
    while (cursor <= content.size()) {
        const std::size_t newline = content.find('\n', cursor);
        const std::string line = Trim(content.substr(cursor, newline == std::string::npos
                                                           ? std::string::npos
                                                           : newline - cursor));
        if (!line.empty() && line[0] != '#') {
            return line;
        }
        if (newline == std::string::npos) {
            break;
        }
        cursor = newline + 1;
    }
    return {};
}

bool ContainsRule(const std::vector<AppRule> &rules, const std::string &packageName, std::uint32_t uid) {
    for (const AppRule &rule : rules) {
        if (rule.packageName == packageName && rule.uid == uid) {
            return true;
        }
    }
    return false;
}

bool SplitLibraries(const std::string &rawValue, std::vector<std::string> *libraries,
                    std::string *error) {
    // Normalize Chinese comma U+FF0C to the same separator used by the English
    // comma. Both are accepted by the WebUI and native parser.
    std::string normalized;
    normalized.reserve(rawValue.size());
    for (std::size_t index = 0; index < rawValue.size();) {
        if (index + 2 < rawValue.size() &&
            static_cast<unsigned char>(rawValue[index]) == 0xEFU &&
            static_cast<unsigned char>(rawValue[index + 1]) == 0xBCU &&
            static_cast<unsigned char>(rawValue[index + 2]) == 0x8CU) {
            normalized.push_back(',');
            index += 3;
        } else {
            normalized.push_back(rawValue[index++]);
        }
    }

    std::size_t cursor = 0;
    while (cursor <= normalized.size()) {
        const std::size_t separator = normalized.find(',', cursor);
        const std::string library = Trim(normalized.substr(cursor, separator == std::string::npos
                                                                     ? std::string::npos
                                                                     : separator - cursor));
        if (!library.empty()) {
            if (!IsSafeLibraryName(library)) {
                *error = "library list contains an invalid library name";
                return false;
            }
            bool duplicate = false;
            for (const std::string &existing : *libraries) {
                if (existing == library) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                libraries->push_back(library);
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        cursor = separator + 1;
    }

    if (libraries->empty()) {
        libraries->emplace_back(kDefaultLibrary);
    }
    return true;
}

bool ParsePerAppConfig(const std::string &content, ModuleConfig *config, std::string *error) {
    ModuleConfig parsed;
    parsed.perAppFormat = true;
    bool seenVersion = false;
    bool seenEnabled = false;

    std::size_t cursor = 0;
    while (cursor <= content.size()) {
        const std::size_t newline = content.find('\n', cursor);
        const std::string line = Trim(content.substr(cursor, newline == std::string::npos
                                                           ? std::string::npos
                                                           : newline - cursor));
        if (!line.empty() && line[0] != '#') {
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos || equals == 0) {
                *error = "invalid per-app configuration line";
                return false;
            }
            const std::string key = Trim(line.substr(0, equals));
            const std::string value = Trim(line.substr(equals + 1));
            if (key == "version") {
                if (seenVersion || value != "3") {
                    *error = "per-app configuration version must be exactly 3";
                    return false;
                }
                seenVersion = true;
            } else if (key == "enabled") {
                if (seenEnabled || !ParseBool(value, &parsed.enabled)) {
                    *error = "enabled must be true or false";
                    return false;
                }
                seenEnabled = true;
            } else if (key == "app") {
                const std::size_t firstDivider = value.find('|');
                const std::size_t secondDivider = firstDivider == std::string::npos
                                                       ? std::string::npos
                                                       : value.find('|', firstDivider + 1);
                const std::size_t thirdDivider = secondDivider == std::string::npos
                                                      ? std::string::npos
                                                      : value.find('|', secondDivider + 1);
                const std::string packageName = Trim(value.substr(0, firstDivider));
                const std::string libraries = firstDivider == std::string::npos
                                                  ? std::string()
                                                  : Trim(value.substr(firstDivider + 1,
                                                                      secondDivider == std::string::npos
                                                                          ? std::string::npos
                                                                          : secondDivider - firstDivider - 1));
                const std::string logValue = secondDivider == std::string::npos
                                                 ? std::string()
                                                 : Trim(value.substr(secondDivider + 1,
                                                                     thirdDivider == std::string::npos
                                                                         ? std::string::npos
                                                                         : thirdDivider - secondDivider - 1));
                const std::string uidValue = thirdDivider == std::string::npos
                                                 ? std::string()
                                                 : Trim(value.substr(thirdDivider + 1));
                if (!IsSafePackageName(packageName)) {
                    *error = "app line contains an invalid package name";
                    return false;
                }
                AppRule rule;
                rule.packageName = packageName;
                if (!SplitLibraries(libraries, &rule.libraries, error)) {
                    return false;
                }
                if (!logValue.empty() && !ParseBool(logValue, &rule.logEnabled)) {
                    *error = "app log value must be true or false";
                    return false;
                }
                if (!uidValue.empty() && !ParseUid(uidValue, &rule.uid)) {
                    *error = "app uid must be a positive unsigned integer";
                    return false;
                }
                if (ContainsRule(parsed.rules, rule.packageName, rule.uid)) {
                    *error = "app line contains a duplicate package and UID rule";
                    return false;
                }
                parsed.rules.push_back(std::move(rule));
            } else {
                *error = "unknown per-app configuration key: " + key;
                return false;
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        cursor = newline + 1;
    }

    if (!seenVersion || !seenEnabled) {
        *error = "per-app configuration is missing version or enabled";
        return false;
    }
    *config = std::move(parsed);
    return true;
}

bool ParseLegacyConfig(const std::string &content, ModuleConfig *config, std::string *error) {
    ModuleConfig parsed;
    bool firstMeaningfulLine = true;
    std::string sharedLibrary;

    std::size_t cursor = 0;
    while (cursor <= content.size()) {
        const std::size_t newline = content.find('\n', cursor);
        const std::string line = Trim(content.substr(cursor, newline == std::string::npos
                                                           ? std::string::npos
                                                           : newline - cursor));
        if (!line.empty() && line[0] != '#') {
            if (firstMeaningfulLine) {
                firstMeaningfulLine = false;
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) {
                    *error = "legacy configuration first line must be enabled:library";
                    return false;
                }
                const std::string enabled = Trim(line.substr(0, colon));
                sharedLibrary = Trim(line.substr(colon + 1));
                if ((enabled != "0" && enabled != "1") || !IsSafeLibraryName(sharedLibrary)) {
                    *error = "legacy configuration has an invalid enabled value or library";
                    return false;
                }
                parsed.enabled = enabled == "1";
            } else {
                if (!IsSafePackageName(line) || ContainsRule(parsed.rules, line, 0U)) {
                    *error = "legacy configuration contains an invalid or duplicate package name";
                    return false;
                }
                parsed.rules.push_back({line, {sharedLibrary}});
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        cursor = newline + 1;
    }

    if (firstMeaningfulLine) {
        *error = "legacy configuration is empty";
        return false;
    }
    *config = std::move(parsed);
    return true;
}

bool LoadConfig(int moduleDirFd, ModuleConfig *config, std::string *error) {
    std::string content;
    std::string readError;
    if (!ReadSmallFileAt(moduleDirFd, kConfigName, &content, &readError)) {
        *error = readError.empty() ? "config.txt does not exist" : readError;
        return false;
    }
    const std::string firstLine = FirstMeaningfulLine(content);
    if (firstLine.rfind("version=", 0) == 0) {
        return ParsePerAppConfig(content, config, error);
    }
    return ParseLegacyConfig(content, config, error);
}

const AppRule *FindRuleByUid(const ModuleConfig &config, std::uint32_t uid) {
    if (uid == 0) return nullptr;
    for (const AppRule &rule : config.rules) {
        if (rule.uid == uid) return &rule;
    }
    return nullptr;
}

const AppRule *FindRuleByPackageAndUid(const ModuleConfig &config, const std::string &packageName,
                                       std::uint32_t uid) {
    if (packageName.empty() || uid == 0) {
        return nullptr;
    }
    for (const AppRule &rule : config.rules) {
        if (rule.packageName == packageName && rule.uid == uid) {
            return &rule;
        }
    }
    return nullptr;
}

void AppendFormat(std::string *output, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int required = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (required > 0) {
        std::vector<char> buffer(static_cast<std::size_t>(required) + 1U);
        std::vsnprintf(buffer.data(), buffer.size(), format, args);
        output->append(buffer.data(), static_cast<std::size_t>(required));
    }
    va_end(args);
}

bool WriteAll(int fd, const std::string &text) {
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = write(fd, text.data() + written, text.size() - written);
        if (count == 0) {
            errno = EIO;
            return false;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

const char *BuildAbiName() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__i386__)
    return "x86";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string JoinLibraries(const std::vector<std::string> &libraries) {
    std::string result;
    for (std::size_t index = 0; index < libraries.size(); ++index) {
        if (index != 0) {
            result += ",";
        }
        result += libraries[index];
    }
    return result;
}

const char *ProtectionText(int protection) {
    // One static thread-local buffer is sufficient because formatting happens
    // synchronously in the current target process.
    static thread_local char text[4];
    text[0] = (protection & PROT_READ) != 0 ? 'r' : '-';
    text[1] = (protection & PROT_WRITE) != 0 ? 'w' : '-';
    text[2] = (protection & PROT_EXEC) != 0 ? 'x' : '-';
    text[3] = '\0';
    return text;
}

std::string ArtifactHexPreview(const std::vector<unsigned char> &bytes, std::size_t offset,
                               std::size_t maximumBytes = 32U) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (offset >= bytes.size()) return {};
    const std::size_t count = std::min(maximumBytes, bytes.size() - offset);
    std::string result;
    result.reserve(count * 3U + 4U);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) result.push_back(' ');
        const unsigned char value = bytes[offset + index];
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    if (offset + count < bytes.size()) result += " ...";
    return result;
}

constexpr std::size_t kMaxLogPayloadBytes = 128U * 1024U;

bool IsSafeLogFileName(const std::string &fileName) {
    if (fileName.size() != 19U || fileName.substr(fileName.size() - 4U) != ".log") {
        return false;
    }
    for (std::size_t index = 0; index < fileName.size(); ++index) {
        const char ch = fileName[index];
        if (index == 8U) {
            if (ch != '-') return false;
        } else if (index >= 15U) {
            if (fileName.substr(index) == ".log") return true;
            return false;
        } else if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return false;
}

bool FormatLogTime(std::string *fileName, std::string *timestamp, std::string *error) {
    const std::time_t now = std::time(nullptr);
    struct tm localTime {};
    if (now == static_cast<std::time_t>(-1) || localtime_r(&now, &localTime) == nullptr) {
        *error = "could not obtain local time for log name";
        return false;
    }
    char fileNameBuffer[96] = {};
    char timestampBuffer[96] = {};
    const int fileNameLength = std::snprintf(fileNameBuffer, sizeof(fileNameBuffer), "%04d%02d%02d-%02d%02d%02d.log",
                                             localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                                             localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    const int timestampLength = std::snprintf(timestampBuffer, sizeof(timestampBuffer), "%04d-%02d-%02dT%02d:%02d:%02d",
                                              localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                                              localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    if (fileNameLength < 0 || timestampLength < 0 ||
        static_cast<std::size_t>(fileNameLength) >= sizeof(fileNameBuffer) ||
        static_cast<std::size_t>(timestampLength) >= sizeof(timestampBuffer)) {
        *error = "could not format log timestamp";
        return false;
    }
    *fileName = fileNameBuffer;
    *timestamp = timestampBuffer;
    return IsSafeLogFileName(*fileName);
}

std::string BuildRestoreLogPayload(const AppRule &rule, const std::string &processName,
                                   const char *recoveryStage, const std::string &timestamp,
                                   const RestoreStats &stats, const std::string &artifactFields = {}) {
    std::string output;
    output.reserve(4096U + stats.segments.size() * 768U);
    AppendFormat(&output, "EVENT_TIME=%s\n", timestamp.c_str());
    AppendFormat(&output, "MODULE=inline_hook_spoof_v2\n");
    AppendFormat(&output, "CONFIG_FORMAT=per_app_v3_or_legacy\n");
    AppendFormat(&output, "ABI=%s\n", BuildAbiName());
    AppendFormat(&output, "PACKAGE=%s\n", rule.packageName.c_str());
    AppendFormat(&output, "PROCESS=%s\n", processName.c_str());
    AppendFormat(&output, "RECOVERY_STAGE=%s\n", recoveryStage == nullptr ? "unknown" : recoveryStage);
    AppendFormat(&output, "RULE_LIBRARIES=%s\n", JoinLibraries(rule.libraries).c_str());
    AppendFormat(&output, "REQUESTED_LIBRARY=%s\n", stats.requestedLibrary.c_str());
    AppendFormat(&output, "RESOLVED_LIBRARY_PATH=%s\n", stats.resolvedLibraryPath.c_str());
    AppendFormat(&output, "LIBRARY_BASE=0x%llX\n", static_cast<unsigned long long>(stats.libraryBase));
    AppendFormat(&output, "LIBRARY_FILE_SIZE=%llu\n", static_cast<unsigned long long>(stats.libraryFileSize));
    AppendFormat(&output, "LIBRARY_DEVICE=%llu\n", static_cast<unsigned long long>(stats.libraryDevice));
    AppendFormat(&output, "LIBRARY_INODE=%llu\n", static_cast<unsigned long long>(stats.libraryInode));
    AppendFormat(&output, "PROGRAM_HEADER_COUNT=%zu\n", stats.programHeaderCount);
    AppendFormat(&output, "OUTCOME=%s\n", RestoreOutcomeName(stats.outcome));
    AppendFormat(&output, "DETAIL=%s\n", stats.detail.c_str());
    AppendFormat(&output, "EXECUTABLE_SEGMENTS=%zu\n", stats.executableSegments);
    AppendFormat(&output, "DIFFERING_SEGMENTS=%zu\n", stats.differingRanges);
    AppendFormat(&output, "TOTAL_DIFFERING_BYTES=%zu\n", stats.differingBytes);
    AppendFormat(&output, "TOTAL_RESTORED_BYTES=%zu\n", stats.restoredBytes);
    AppendFormat(&output, "ROLLBACK_ATTEMPTED=%s\n", stats.rollbackAttempted ? "true" : "false");
    AppendFormat(&output, "ROLLBACK_SUCCEEDED=%s\n", stats.rollbackSucceeded ? "true" : "false");

    for (const RestoreSegmentRecord &segment : stats.segments) {
        const std::string prefix = "SEGMENT_" + std::to_string(segment.segmentIndex) + "_";
        AppendFormat(&output, "%sMEMORY_ADDRESS=0x%llX\n", prefix.c_str(),
                     static_cast<unsigned long long>(segment.memoryAddress));
        AppendFormat(&output, "%sFILE_OFFSET=0x%zX\n", prefix.c_str(), segment.fileOffset);
        AppendFormat(&output, "%sSIZE=%zu\n", prefix.c_str(), segment.size);
        AppendFormat(&output, "%sPAGE_START=0x%llX\n", prefix.c_str(),
                     static_cast<unsigned long long>(segment.pageStart));
        AppendFormat(&output, "%sPAGE_LENGTH=%zu\n", prefix.c_str(), segment.pageLength);
        AppendFormat(&output, "%sORIGINAL_PROTECTION=%s\n", prefix.c_str(),
                     ProtectionText(segment.originalProtection));
        AppendFormat(&output, "%sHAS_DIFFERENCE=%s\n", prefix.c_str(),
                     segment.hasDifference ? "true" : "false");
        AppendFormat(&output, "%sDIFFERING_BYTES=%zu\n", prefix.c_str(), segment.differingBytes);
        AppendFormat(&output, "%sDIFFERENCE_RANGE_COUNT=%zu\n", prefix.c_str(), segment.differenceRanges.size());
        for (std::size_t rangeIndex = 0; rangeIndex < segment.differenceRanges.size(); ++rangeIndex) {
            const RestoreDifferenceRange &range = segment.differenceRanges[rangeIndex];
            const std::string rangePrefix = prefix + "RANGE_" + std::to_string(rangeIndex) + "_";
            AppendFormat(&output, "%sSEGMENT_OFFSET=0x%zX\n", rangePrefix.c_str(), range.offset);
            AppendFormat(&output, "%sFILE_OFFSET=0x%zX\n", rangePrefix.c_str(), segment.fileOffset + range.offset);
            AppendFormat(&output, "%sMEMORY_ADDRESS=0x%llX\n", rangePrefix.c_str(),
                         static_cast<unsigned long long>(segment.memoryAddress + range.offset));
            AppendFormat(&output, "%sLENGTH=%zu\n", rangePrefix.c_str(), range.length);
            if (!segment.artifactBeforeBytes.empty() && !segment.artifactAfterBytes.empty()) {
                AppendFormat(&output, "%sBEFORE_PREVIEW=%s\n", rangePrefix.c_str(),
                             ArtifactHexPreview(segment.artifactBeforeBytes, range.offset).c_str());
                AppendFormat(&output, "%sAFTER_PREVIEW=%s\n", rangePrefix.c_str(),
                             ArtifactHexPreview(segment.artifactAfterBytes, range.offset).c_str());
            }
        }
        if (segment.hasDifference) {
            AppendFormat(&output, "%sFIRST_DIFF_OFFSET=0x%zX\n", prefix.c_str(),
                         segment.firstDifferenceOffset);
            AppendFormat(&output, "%sLAST_DIFF_OFFSET=0x%zX\n", prefix.c_str(),
                         segment.lastDifferenceOffset);
            AppendFormat(&output, "%sFIRST_DIFF_MEMORY_ADDRESS=0x%llX\n", prefix.c_str(),
                         static_cast<unsigned long long>(segment.memoryAddress + segment.firstDifferenceOffset));
            AppendFormat(&output, "%sDISK_BYTES_AT_FIRST_DIFF=%s\n", prefix.c_str(),
                         segment.diskBytesAtFirstDifference.c_str());
            AppendFormat(&output, "%sMEMORY_BYTES_AT_FIRST_DIFF=%s\n", prefix.c_str(),
                         segment.memoryBytesAtFirstDifference.c_str());
        }
        AppendFormat(&output, "%sWRITE_ATTEMPTED=%s\n", prefix.c_str(),
                     segment.writeAttempted ? "true" : "false");
        AppendFormat(&output, "%sUSED_RWX_WRITE=%s\n", prefix.c_str(),
                     segment.usedRwxWrite ? "true" : "false");
        AppendFormat(&output, "%sBYTE_VERIFIED=%s\n", prefix.c_str(),
                     segment.byteVerified ? "true" : "false");
        AppendFormat(&output, "%sPROTECTION_RESTORED=%s\n", prefix.c_str(),
                     segment.protectionRestored ? "true" : "false");
        AppendFormat(&output, "%sRESTORED=%s\n", prefix.c_str(), segment.restored ? "true" : "false");
        AppendFormat(&output, "%sROLLBACK_ATTEMPTED=%s\n", prefix.c_str(),
                     segment.rollbackAttempted ? "true" : "false");
        AppendFormat(&output, "%sROLLBACK_SUCCEEDED=%s\n", prefix.c_str(),
                     segment.rollbackSucceeded ? "true" : "false");
        if (!segment.failure.empty()) {
            AppendFormat(&output, "%sFAILURE=%s\n", prefix.c_str(), segment.failure.c_str());
        }
    }
    if (!artifactFields.empty()) {
        output += artifactFields;
        if (artifactFields.back() != '\n') output.push_back('\n');
    }
    output += "END_EVENT\n\n";
    return output;
}

bool WriteLogPayloadAt(int moduleDirFd, const std::string &packageName, std::uint32_t uid,
                       const std::string &fileName, const std::string &payload, std::string *error) {
    if (!IsSafePackageName(packageName) || uid == 0 || !IsSafeLogFileName(fileName) || payload.empty() ||
        payload.size() > kMaxLogPayloadBytes) {
        *error = "invalid log payload";
        return false;
    }
    if (mkdirat(moduleDirFd, "logs", 0755) != 0 && errno != EEXIST) {
        *error = std::string("mkdir logs failed: ") + std::strerror(errno);
        return false;
    }
    const int logsFd = openat(moduleDirFd, "logs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (logsFd < 0) {
        *error = std::string("open logs directory failed: ") + std::strerror(errno);
        return false;
    }
    const std::string instanceDirectory = packageName + "_" + std::to_string(uid);
    if (mkdirat(logsFd, instanceDirectory.c_str(), 0755) != 0 && errno != EEXIST) {
        const int savedErrno = errno;
        close(logsFd);
        *error = std::string("mkdir package UID log directory failed: ") + std::strerror(savedErrno);
        return false;
    }
    const int packageFd = openat(logsFd, instanceDirectory.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(logsFd);
    if (packageFd < 0) {
        *error = std::string("open package log directory failed: ") + std::strerror(errno);
        return false;
    }
    const int logFd = openat(packageFd, fileName.c_str(),
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    close(packageFd);
    if (logFd < 0) {
        *error = std::string("open log file failed: ") + std::strerror(errno);
        return false;
    }
    const bool written = WriteAll(logFd, payload);
    const int savedErrno = errno;
    close(logFd);
    if (!written) {
        *error = std::string("write log file failed: ") + std::strerror(savedErrno);
    }
    return written;
}

constexpr std::uint32_t kCompanionLogMagic = 0x4948534CU;  // "IHSL"
constexpr std::uint32_t kCompanionArtifactMagic = 0x49485341U;  // "IHSA"
constexpr std::size_t kMaxArtifactManifestBytes = 256U * 1024U;
constexpr std::size_t kMaxArtifactSnapshotBytes = 64U * 1024U * 1024U;

struct CompanionArtifactHeader {
    std::uint32_t magic = kCompanionArtifactMagic;
    std::uint32_t uid = 0;
    std::uint32_t packageLength = 0;
    std::uint32_t libraryNameLength = 0;
    std::uint32_t sourcePathLength = 0;
    std::uint32_t artifactIdLength = 0;
    std::uint32_t manifestLength = 0;
    std::uint32_t segmentCount = 0;
    std::uint64_t totalSnapshotBytes = 0;
};

struct CompanionArtifactSegmentHeader {
    std::uint64_t memoryAddress = 0;
    std::uint64_t fileOffset = 0;
    std::uint64_t size = 0;
};

struct ArtifactCaptureResult {
    bool attempted = false;
    bool created = false;
    std::string relativeDirectory;
    std::string error;
};

struct CompanionLogHeader {
    std::uint32_t magic = kCompanionLogMagic;
    std::uint32_t uid = 0;
    std::uint32_t packageLength = 0;
    std::uint32_t fileNameLength = 0;
    std::uint32_t payloadLength = 0;
};

bool SendAll(int socketFd, const void *buffer, std::size_t size) {
    const char *cursor = static_cast<const char *>(buffer);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count = send(socketFd, cursor + sent, size - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) {
            errno = EPIPE;
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool IsSafeArtifactComponent(const std::string &value) {
    if (value.empty() || value.size() > 120U) return false;
    for (const unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) return false;
    }
    return true;
}

bool IsSafeAbsoluteArtifactSource(const std::string &path) {
    return !path.empty() && path.front() == '/' && path.find("..") == std::string::npos && path.size() <= 1024U;
}

bool WriteAllBytes(int fd, const void *buffer, std::size_t size) {
    const unsigned char *cursor = static_cast<const unsigned char *>(buffer);
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t written = write(fd, cursor + completed, size - completed);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        completed += static_cast<std::size_t>(written);
    }
    return true;
}

bool PwriteAllBytes(int fd, const void *buffer, std::size_t size, off_t offset) {
    const unsigned char *cursor = static_cast<const unsigned char *>(buffer);
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t written = pwrite(fd, cursor + completed, size - completed,
                                       offset + static_cast<off_t>(completed));
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        completed += static_cast<std::size_t>(written);
    }
    return true;
}

bool CopyFileToFd(const std::string &sourcePath, int destinationFd, std::string *error) {
    const int sourceFd = open(sourcePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (sourceFd < 0) {
        *error = std::string("open artifact source failed: ") + std::strerror(errno);
        return false;
    }
    std::vector<unsigned char> buffer(64U * 1024U);
    bool copied = true;
    while (true) {
        const ssize_t count = read(sourceFd, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            copied = false;
            *error = std::string("read artifact source failed: ") + std::strerror(errno);
            break;
        }
        if (!WriteAllBytes(destinationFd, buffer.data(), static_cast<std::size_t>(count))) {
            copied = false;
            *error = std::string("write artifact copy failed: ") + std::strerror(errno);
            break;
        }
    }
    close(sourceFd);
    return copied;
}

bool ReceiveAll(int socketFd, void *buffer, std::size_t size) {
    char *cursor = static_cast<char *>(buffer);
    std::size_t received = 0;
    while (received < size) {
        const ssize_t count = recv(socketFd, cursor + received, size - received, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) {
            errno = ECONNRESET;
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

bool ReceiveString(int socketFd, std::size_t size, std::string *output) {
    output->assign(size, '\0');
    return size == 0 || ReceiveAll(socketFd, output->data(), size);
}

bool SendModuleDirFd(int socketFd, int moduleDirFd) {
    char markerByte = 'D';
    struct iovec iov { &markerByte, sizeof(markerByte) };
    char control[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &moduleDirFd, sizeof(moduleDirFd));
    message.msg_controllen = CMSG_SPACE(sizeof(int));
    return sendmsg(socketFd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(markerByte));
}

int ReceiveModuleDirFd(int socketFd) {
    char markerByte = 0;
    struct iovec iov { &markerByte, sizeof(markerByte) };
    char control[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socketFd, &message, 0) != static_cast<ssize_t>(sizeof(markerByte)) || markerByte != 'D') {
        return -1;
    }
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(header), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

bool SendCompanionLog(int socketFd, const std::string &packageName, std::uint32_t uid,
                      const std::string &fileName, const std::string &payload, std::string *error) {
    if (!IsSafePackageName(packageName) || uid == 0 || !IsSafeLogFileName(fileName) || payload.empty() ||
        payload.size() > kMaxLogPayloadBytes) {
        *error = "invalid companion log payload";
        return false;
    }
    CompanionLogHeader header;
    header.uid = uid;
    header.packageLength = static_cast<std::uint32_t>(packageName.size());
    header.fileNameLength = static_cast<std::uint32_t>(fileName.size());
    header.payloadLength = static_cast<std::uint32_t>(payload.size());
    if (!SendAll(socketFd, &header, sizeof(header)) ||
        !SendAll(socketFd, packageName.data(), packageName.size()) ||
        !SendAll(socketFd, fileName.data(), fileName.size()) ||
        !SendAll(socketFd, payload.data(), payload.size())) {
        *error = std::string("send companion log failed: ") + std::strerror(errno);
        return false;
    }
    std::uint32_t status = 0;
    if (!ReceiveAll(socketFd, &status, sizeof(status))) {
        *error = std::string("read companion result failed: ") + std::strerror(errno);
        return false;
    }
    if (status != 1U) {
        *error = "root companion rejected log write";
        return false;
    }
    return true;
}

bool WriteLogThroughCompanion(zygisk::Api *api, int moduleDirFd, const AppRule &rule,
                              const std::string &processName, const RestoreStats &stats,
                              const std::string &artifactFields, std::string *error) {
    std::string fileName;
    std::string timestamp;
    if (!FormatLogTime(&fileName, &timestamp, error)) {
        return false;
    }
    const int socketFd = api->connectCompanion();
    if (socketFd < 0) {
        *error = "connectCompanion failed";
        return false;
    }
    const bool fdSent = SendModuleDirFd(socketFd, moduleDirFd);
    bool logged = false;
    if (fdSent) {
        logged = SendCompanionLog(socketFd, rule.packageName, rule.uid, fileName,
                                  BuildRestoreLogPayload(rule, processName,
                                                         "preAppSpecialize_package_uid_root_companion",
                                                         timestamp, stats, artifactFields), error);
    } else {
        *error = std::string("send module directory FD failed: ") + std::strerror(errno);
    }
    shutdown(socketFd, SHUT_WR);
    close(socketFd);
    return logged;
}

std::string ArtifactLibraryComponent(const std::string &library) {
    std::string component = library;
    const std::size_t slash = component.find_last_of('/');
    if (slash != std::string::npos) component.erase(0, slash + 1U);
    for (char &ch : component) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!(std::isalnum(value) || ch == '.' || ch == '_' || ch == '-')) ch = '_';
    }
    return component.empty() ? "library" : component;
}

std::string BuildArtifactManifest(const AppRule &rule, const std::string &processName,
                                  const std::string &artifactDirectory, const RestoreStats &stats) {
    std::string output;
    output.reserve(8192U);
    AppendFormat(&output, "PACKAGE=%s\n", rule.packageName.c_str());
    AppendFormat(&output, "UID=%u\n", rule.uid);
    AppendFormat(&output, "PROCESS=%s\n", processName.c_str());
    AppendFormat(&output, "ARTIFACT_DIRECTORY=%s\n", artifactDirectory.c_str());
    AppendFormat(&output, "REQUESTED_LIBRARY=%s\n", stats.requestedLibrary.c_str());
    AppendFormat(&output, "RESOLVED_LIBRARY_PATH=%s\n", stats.resolvedLibraryPath.c_str());
    AppendFormat(&output, "LIBRARY_BASE=0x%llX\n", static_cast<unsigned long long>(stats.libraryBase));
    AppendFormat(&output, "TOTAL_DIFFERING_BYTES=%zu\n", stats.differingBytes);
    AppendFormat(&output, "DIFF_RANGES_FILE=diff-ranges.txt\n");
    AppendFormat(&output, "SO_FILES=disk-original.so,before.so,after.so\n");
    return output;
}

ArtifactCaptureResult CaptureArtifactThroughCompanion(zygisk::Api *api, int moduleDirFd,
                                                       const AppRule &rule, const std::string &processName,
                                                       const RestoreStats &stats) {
    ArtifactCaptureResult result;
    if (!rule.logEnabled || stats.differingBytes == 0 || stats.resolvedLibraryPath.empty()) {
        return result;
    }
    std::vector<const RestoreSegmentRecord *> segments;
    std::size_t totalSnapshotBytes = 0;
    for (const RestoreSegmentRecord &segment : stats.segments) {
        if (!segment.hasDifference) continue;
        if (segment.artifactBeforeBytes.empty() || segment.artifactAfterBytes.size() != segment.artifactBeforeBytes.size()) {
            result.attempted = true;
            result.error = "artifact snapshots unavailable for a differing executable segment";
            return result;
        }
        if (segment.artifactBeforeBytes.size() > kMaxArtifactSnapshotBytes - totalSnapshotBytes) {
            result.attempted = true;
            result.error = "artifact snapshot budget exceeded";
            return result;
        }
        totalSnapshotBytes += segment.artifactBeforeBytes.size();
        segments.push_back(&segment);
    }
    if (segments.empty()) return result;
    std::string logFileName;
    std::string timestamp;
    if (!FormatLogTime(&logFileName, &timestamp, &result.error)) return result;
    const std::string libraryComponent = ArtifactLibraryComponent(stats.requestedLibrary);
    const std::string artifactId = logFileName.substr(0, logFileName.size() - 4U) + "_" +
                                   libraryComponent;
    const std::string instanceDirectory = rule.packageName + "_" + std::to_string(rule.uid);
    result.relativeDirectory = "artifacts/" + instanceDirectory + "/" + artifactId;
    const std::string manifest = BuildArtifactManifest(rule, processName, result.relativeDirectory, stats);
    if (manifest.size() > kMaxArtifactManifestBytes || !IsSafeArtifactComponent(artifactId) ||
        !IsSafeAbsoluteArtifactSource(stats.resolvedLibraryPath)) {
        result.attempted = true;
        result.error = "artifact metadata failed safety validation";
        return result;
    }
    result.attempted = true;
    const int socketFd = api->connectCompanion();
    if (socketFd < 0) {
        result.error = "connectCompanion failed for artifact capture";
        return result;
    }
    bool sent = SendModuleDirFd(socketFd, moduleDirFd);
    CompanionArtifactHeader header;
    header.uid = rule.uid;
    header.packageLength = static_cast<std::uint32_t>(rule.packageName.size());
    header.libraryNameLength = static_cast<std::uint32_t>(libraryComponent.size());
    header.sourcePathLength = static_cast<std::uint32_t>(stats.resolvedLibraryPath.size());
    header.artifactIdLength = static_cast<std::uint32_t>(artifactId.size());
    header.manifestLength = static_cast<std::uint32_t>(manifest.size());
    header.segmentCount = static_cast<std::uint32_t>(segments.size());
    header.totalSnapshotBytes = totalSnapshotBytes;
    sent = sent && SendAll(socketFd, &header, sizeof(header)) &&
           SendAll(socketFd, rule.packageName.data(), rule.packageName.size()) &&
           SendAll(socketFd, libraryComponent.data(), libraryComponent.size()) &&
           SendAll(socketFd, stats.resolvedLibraryPath.data(), stats.resolvedLibraryPath.size()) &&
           SendAll(socketFd, artifactId.data(), artifactId.size()) &&
           SendAll(socketFd, manifest.data(), manifest.size());
    for (const RestoreSegmentRecord *segment : segments) {
        CompanionArtifactSegmentHeader segmentHeader;
        segmentHeader.memoryAddress = segment->memoryAddress;
        segmentHeader.fileOffset = segment->fileOffset;
        segmentHeader.size = segment->artifactBeforeBytes.size();
        sent = sent && SendAll(socketFd, &segmentHeader, sizeof(segmentHeader)) &&
               SendAll(socketFd, segment->artifactBeforeBytes.data(), segment->artifactBeforeBytes.size()) &&
               SendAll(socketFd, segment->artifactAfterBytes.data(), segment->artifactAfterBytes.size());
    }
    std::uint32_t response = 0;
    if (!sent || !ReceiveAll(socketFd, &response, sizeof(response)) || response != 1U) {
        result.error = sent ? "root companion rejected artifact capture" :
                              std::string("send artifact capture failed: ") + std::strerror(errno);
    } else {
        result.created = true;
    }
    shutdown(socketFd, SHUT_WR);
    close(socketFd);
    return result;
}

bool EnsureDirectoryAt(int parentFd, const std::string &name, int *directoryFd, std::string *error) {
    if (!IsSafeArtifactComponent(name)) {
        *error = "unsafe artifact directory component";
        return false;
    }
    if (mkdirat(parentFd, name.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = std::string("mkdir artifact directory failed: ") + std::strerror(errno);
        return false;
    }
    *directoryFd = openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (*directoryFd < 0) {
        *error = std::string("open artifact directory failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool WriteArtifactDifferenceRanges(int fd, const CompanionArtifactSegmentHeader &header,
                                   const std::vector<unsigned char> &before,
                                   const std::vector<unsigned char> &after) {
    if (before.size() != after.size()) return false;
    bool inRange = false;
    std::size_t rangeStart = 0;
    std::size_t rangeIndex = 0;
    for (std::size_t index = 0; index <= before.size(); ++index) {
        const bool differs = index < before.size() && before[index] != after[index];
        if (differs && !inRange) {
            rangeStart = index;
            inRange = true;
        }
        if ((!differs || index == before.size()) && inRange) {
            const std::size_t length = index - rangeStart;
            std::string text;
            AppendFormat(&text, "RANGE_INDEX=%zu\n", rangeIndex++);
            AppendFormat(&text, "SEGMENT_FILE_OFFSET=0x%llX\n",
                         static_cast<unsigned long long>(header.fileOffset));
            AppendFormat(&text, "SEGMENT_MEMORY_ADDRESS=0x%llX\n",
                         static_cast<unsigned long long>(header.memoryAddress));
            AppendFormat(&text, "FILE_OFFSET=0x%llX\n",
                         static_cast<unsigned long long>(header.fileOffset + rangeStart));
            AppendFormat(&text, "MEMORY_ADDRESS=0x%llX\n",
                         static_cast<unsigned long long>(header.memoryAddress + rangeStart));
            AppendFormat(&text, "LENGTH=%zu\n", length);
            AppendFormat(&text, "BEFORE_PREVIEW=%s\n", ArtifactHexPreview(before, rangeStart).c_str());
            AppendFormat(&text, "AFTER_PREVIEW=%s\n\n", ArtifactHexPreview(after, rangeStart).c_str());
            if (!WriteAll(fd, text)) return false;
            inRange = false;
        }
    }
    return true;
}

bool WriteArtifactStream(int client, int moduleDirFd, CompanionArtifactHeader header, std::string *error) {
    if (header.uid == 0U || header.packageLength == 0U || header.packageLength > 255U ||
        header.libraryNameLength == 0U || header.libraryNameLength > 255U ||
        header.sourcePathLength == 0U || header.sourcePathLength > 1024U ||
        header.artifactIdLength == 0U || header.artifactIdLength > 120U ||
        header.manifestLength == 0U || header.manifestLength > kMaxArtifactManifestBytes ||
        header.segmentCount == 0U || header.segmentCount > 64U ||
        header.totalSnapshotBytes == 0U || header.totalSnapshotBytes > kMaxArtifactSnapshotBytes) {
        *error = "invalid artifact header";
        return false;
    }
    std::string packageName;
    std::string libraryName;
    std::string sourcePath;
    std::string artifactId;
    std::string manifest;
    if (!ReceiveString(client, header.packageLength, &packageName) ||
        !ReceiveString(client, header.libraryNameLength, &libraryName) ||
        !ReceiveString(client, header.sourcePathLength, &sourcePath) ||
        !ReceiveString(client, header.artifactIdLength, &artifactId) ||
        !ReceiveString(client, header.manifestLength, &manifest)) {
        *error = "artifact metadata receive failed";
        return false;
    }
    if (!IsSafePackageName(packageName) || !IsSafeArtifactComponent(libraryName) ||
        !IsSafeArtifactComponent(artifactId) || !IsSafeAbsoluteArtifactSource(sourcePath)) {
        *error = "artifact metadata safety validation failed";
        return false;
    }

    const int sourceFd = open(sourcePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (sourceFd < 0) {
        *error = std::string("open artifact source failed: ") + std::strerror(errno);
        return false;
    }
    struct stat sourceStat {};
    if (fstat(sourceFd, &sourceStat) != 0 || !S_ISREG(sourceStat.st_mode) || sourceStat.st_size <= 0) {
        const int savedErrno = errno;
        close(sourceFd);
        *error = std::string("invalid artifact source file: ") + std::strerror(savedErrno);
        return false;
    }
    close(sourceFd);

    int artifactsFd = -1;
    int instanceFd = -1;
    int temporaryFd = -1;
    const std::string instanceDirectory = packageName + "_" + std::to_string(header.uid);
    const std::string temporaryDirectory = "." + artifactId + ".tmp";
    bool success = EnsureDirectoryAt(moduleDirFd, "artifacts", &artifactsFd, error) &&
                   EnsureDirectoryAt(artifactsFd, instanceDirectory, &instanceFd, error);
    if (success) {
        if (mkdirat(instanceFd, temporaryDirectory.c_str(), 0755) != 0) {
            *error = std::string("mkdir temporary artifact directory failed: ") + std::strerror(errno);
            success = false;
        } else {
            temporaryFd = openat(instanceFd, temporaryDirectory.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (temporaryFd < 0) {
                *error = std::string("open temporary artifact directory failed: ") + std::strerror(errno);
                success = false;
            }
        }
    }

    const char *const names[] = {"disk-original.so", "before.so", "after.so"};
    int outputs[3] = {-1, -1, -1};
    if (success) {
        for (std::size_t index = 0; index < 3U; ++index) {
            outputs[index] = openat(temporaryFd, names[index], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
            if (outputs[index] < 0 || !CopyFileToFd(sourcePath, outputs[index], error)) {
                if (outputs[index] < 0) *error = std::string("open artifact SO failed: ") + std::strerror(errno);
                success = false;
                break;
            }
            close(outputs[index]);
            outputs[index] = -1;
        }
    }

    if (success) {
        const int beforeFd = openat(temporaryFd, "before.so", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        const int afterFd = openat(temporaryFd, "after.so", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        const int rangesFd = openat(temporaryFd, "diff-ranges.txt",
                                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (beforeFd < 0 || afterFd < 0 || rangesFd < 0) {
            *error = std::string("open artifact overlays or ranges failed: ") + std::strerror(errno);
            success = false;
        }
        std::uint64_t receivedSnapshotBytes = 0;
        for (std::size_t index = 0; success && index < header.segmentCount; ++index) {
            CompanionArtifactSegmentHeader segmentHeader {};
            if (!ReceiveAll(client, &segmentHeader, sizeof(segmentHeader)) || segmentHeader.size == 0U ||
                segmentHeader.size > kMaxArtifactSnapshotBytes - receivedSnapshotBytes ||
                segmentHeader.fileOffset > static_cast<std::uint64_t>(sourceStat.st_size) ||
                segmentHeader.size > static_cast<std::uint64_t>(sourceStat.st_size) - segmentHeader.fileOffset ||
                segmentHeader.fileOffset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                *error = "invalid artifact segment header";
                success = false;
                break;
            }
            std::vector<unsigned char> before(static_cast<std::size_t>(segmentHeader.size));
            std::vector<unsigned char> after(static_cast<std::size_t>(segmentHeader.size));
            if (!ReceiveAll(client, before.data(), before.size()) || !ReceiveAll(client, after.data(), after.size()) ||
                !PwriteAllBytes(beforeFd, before.data(), before.size(), static_cast<off_t>(segmentHeader.fileOffset)) ||
                !PwriteAllBytes(afterFd, after.data(), after.size(), static_cast<off_t>(segmentHeader.fileOffset)) ||
                !WriteArtifactDifferenceRanges(rangesFd, segmentHeader, before, after)) {
                *error = std::string("artifact segment write or range report failed: ") + std::strerror(errno);
                success = false;
                break;
            }
            receivedSnapshotBytes += segmentHeader.size;
        }
        if (receivedSnapshotBytes != header.totalSnapshotBytes) {
            *error = "artifact snapshot byte count mismatch";
            success = false;
        }
        if (beforeFd >= 0) close(beforeFd);
        if (afterFd >= 0) close(afterFd);
        if (rangesFd >= 0) close(rangesFd);
    }

    if (success) {
        const int manifestFd = openat(temporaryFd, "manifest.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (manifestFd < 0 || !WriteAll(manifestFd, manifest)) {
            *error = std::string("write artifact manifest failed: ") + std::strerror(errno);
            success = false;
        }
        if (manifestFd >= 0) close(manifestFd);
    }
    if (success && renameat(instanceFd, temporaryDirectory.c_str(), instanceFd, artifactId.c_str()) != 0) {
        *error = std::string("publish artifact directory failed: ") + std::strerror(errno);
        success = false;
    }
    if (temporaryFd >= 0) close(temporaryFd);
    if (instanceFd >= 0) close(instanceFd);
    if (artifactsFd >= 0) close(artifactsFd);
    return success;
}

void CompanionHandler(int client) {
    const int moduleDirFd = ReceiveModuleDirFd(client);
    if (moduleDirFd < 0) {
        close(client);
        return;
    }
    while (true) {
        std::uint32_t magic = 0;
        if (!ReceiveAll(client, &magic, sizeof(magic))) break;
        bool written = false;
        std::string error;
        if (magic == kCompanionLogMagic) {
            CompanionLogHeader header {};
            header.magic = magic;
            const bool headerRead = ReceiveAll(client,
                                                reinterpret_cast<unsigned char *>(&header) + sizeof(header.magic),
                                                sizeof(header) - sizeof(header.magic));
            bool valid = headerRead && header.uid > 0U && header.packageLength > 0U &&
                         header.packageLength <= 255U && header.fileNameLength > 0U &&
                         header.fileNameLength <= 95U && header.payloadLength > 0U &&
                         header.payloadLength <= kMaxLogPayloadBytes;
            std::string packageName;
            std::string fileName;
            std::string payload;
            if (valid) {
                valid = ReceiveString(client, header.packageLength, &packageName) &&
                        ReceiveString(client, header.fileNameLength, &fileName) &&
                        ReceiveString(client, header.payloadLength, &payload);
            }
            written = valid && WriteLogPayloadAt(moduleDirFd, packageName, header.uid, fileName, payload, &error);
        } else if (magic == kCompanionArtifactMagic) {
            CompanionArtifactHeader header {};
            header.magic = magic;
            const bool headerRead = ReceiveAll(client,
                                                reinterpret_cast<unsigned char *>(&header) + sizeof(header.magic),
                                                sizeof(header) - sizeof(header.magic));
            written = headerRead && WriteArtifactStream(client, moduleDirFd, header, &error);
        } else {
            error = "unknown root companion request magic";
        }
        const std::uint32_t response = written ? 1U : 0U;
        if (!SendAll(client, &response, sizeof(response))) break;
    }
    close(moduleDirFd);
    close(client);
}

class Module final : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        if (env != nullptr) {
            env->GetJavaVM(&vm_);
        }
        if (api_ != nullptr) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    // Preserve the original module's pre-specialization package timing and JNI
    // path so restoration completes before LSP installs ART hooks. UID is an
    // added second key for clone/user separation, not a package-name replacement.
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (api_ == nullptr || vm_ == nullptr || args == nullptr || args->nice_name == nullptr || args->uid <= 0) {
            return;
        }
        const std::uint32_t actualUid = static_cast<std::uint32_t>(args->uid);
        const int moduleDirFd = api_->getModuleDir();
        if (moduleDirFd < 0) {
            return;
        }
        ModuleConfig config;
        std::string error;
        const bool configLoaded = LoadConfig(moduleDirFd, &config, &error);
        const AppRule *uidRule = configLoaded && config.enabled ? FindRuleByUid(config, actualUid) : nullptr;
        if (uidRule == nullptr) {
            close(moduleDirFd);
            return;
        }
        LOGI("V2: UID_PRE_MATCH=true configured_package=%s configured_uid=%u actual_uid=%u",
             uidRule->packageName.c_str(), uidRule->uid, actualUid);

        JNIEnv *env = nullptr;
        bool attachedHere = false;
        const jint environmentResult = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (environmentResult == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                LOGE("V2: UID_PRE_MATCH=true but AttachCurrentThread failed actual_uid=%u", actualUid);
                close(moduleDirFd);
                return;
            }
            attachedHere = true;
        } else if (environmentResult != JNI_OK || env == nullptr) {
            LOGE("V2: UID_PRE_MATCH=true but GetEnv failed actual_uid=%u", actualUid);
            close(moduleDirFd);
            return;
        }
        const char *rawName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (rawName == nullptr) {
            if (attachedHere) vm_->DetachCurrentThread();
            LOGE("V2: UID_PRE_MATCH=true but GetStringUTFChars failed actual_uid=%u", actualUid);
            close(moduleDirFd);
            return;
        }
        const std::string processName(rawName);
        env->ReleaseStringUTFChars(args->nice_name, rawName);
        if (attachedHere) vm_->DetachCurrentThread();

        const AppRule *rule = FindRuleByPackageAndUid(config, processName, actualUid);
        if (rule == nullptr) {
            LOGI("V2: PACKAGE_PRE_MATCH=false configured_package=%s actual_name=%s actual_uid=%u",
                 uidRule->packageName.c_str(), processName.c_str(), actualUid);
            close(moduleDirFd);
            return;
        }
        LOGI("V2: PACKAGE_PRE_MATCH=true package=%s uid=%u", rule->packageName.c_str(), rule->uid);

        for (const std::string &library : rule->libraries) {
            LOGI("V2: RESTORE_ATTEMPTED=true package=%s uid=%u library=%s",
                 rule->packageName.c_str(), rule->uid, library.c_str());
            RestoreConfig restore;
            restore.libraryName = library;
            restore.captureArtifactSnapshots = rule->logEnabled;
            const RestoreStats stats = InspectOrRestoreLibrary(restore);
            LOGI("V2: %s stage=preAppSpecialize_package_uid package=%s uid=%u library=%s segments=%zu ranges=%zu differing=%zu restored=%zu detail=%s",
                 RestoreOutcomeName(stats.outcome), rule->packageName.c_str(), rule->uid,
                 stats.requestedLibrary.c_str(), stats.executableSegments, stats.differingRanges,
                 stats.differingBytes, stats.restoredBytes, stats.detail.c_str());
            if (rule->logEnabled) {
                const ArtifactCaptureResult artifact =
                    CaptureArtifactThroughCompanion(api_, moduleDirFd, *rule, processName, stats);
                std::string artifactFields;
                if (stats.differingBytes == 0) {
                    artifactFields = "ARTIFACT_STATUS=not_required_no_difference\n";
                } else if (artifact.created) {
                    artifactFields = "ARTIFACT_STATUS=created\nARTIFACT_DIRECTORY=" +
                                     artifact.relativeDirectory + "\n";
                } else {
                    artifactFields = "ARTIFACT_STATUS=failed\nARTIFACT_ERROR=" + artifact.error + "\n";
                }
                std::string logError;
                if (!WriteLogThroughCompanion(api_, moduleDirFd, *rule, processName, stats,
                                              artifactFields, &logError)) {
                    LOGE("V2: root companion log write failed for %s: %s", rule->packageName.c_str(),
                         logError.c_str());
                }
            }
        }
        close(moduleDirFd);
    }

    // All restoration and logging complete in preAppSpecialize. Keeping this
    // empty prevents post-LSP ART page writes and post-stage log dependencies.
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {}

private:
    zygisk::Api *api_ = nullptr;
    JavaVM *vm_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(Module)
REGISTER_ZYGISK_COMPANION(CompanionHandler)
