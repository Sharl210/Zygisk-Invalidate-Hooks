#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../module/src/main/cpp/main.cpp"

RestoreStats InspectOrRestoreLibrary(const RestoreConfig &) { return {}; }
const char *RestoreModeName(RestoreMode) { return "restore"; }
const char *RestoreOutcomeName(RestoreOutcome) { return "restored"; }

namespace {
[[noreturn]] void Fail(const char *message) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
void Require(bool condition, const char *message) { if (!condition) Fail(message); }
std::vector<unsigned char> ReadBytes(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    Require(fd >= 0, "could not open artifact file");
    std::vector<unsigned char> data;
    std::array<unsigned char, 512> buffer{};
    while (true) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0) break;
        Require(count > 0, "could not read artifact file");
        data.insert(data.end(), buffer.begin(), buffer.begin() + count);
    }
    close(fd);
    return data;
}
std::string ReadText(const std::string &path) {
    const auto bytes = ReadBytes(path);
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
}  // namespace

int main() {
    char rootTemplate[] = "/tmp/inline-hook-spoof-artifacts-XXXXXX";
    char *root = mkdtemp(rootTemplate);
    Require(root != nullptr, "mkdtemp root failed");
    char sourceTemplate[] = "/tmp/inline-hook-spoof-source-XXXXXX";
    const int sourceFd = mkstemp(sourceTemplate);
    Require(sourceFd >= 0, "mkstemp source failed");
    std::vector<unsigned char> disk(4096U);
    for (std::size_t index = 0; index < disk.size(); ++index) disk[index] = static_cast<unsigned char>(index & 0xFFU);
    Require(WriteAllBytes(sourceFd, disk.data(), disk.size()), "could not seed source SO");
    close(sourceFd);

    const int rootFd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(rootFd >= 0, "could not open artifact root");
    int sockets[2] = {-1, -1};
    Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    std::thread companion([&] { CompanionHandler(sockets[1]); });
    Require(SendModuleDirFd(sockets[0], rootFd), "could not pass artifact root FD");
    close(rootFd);

    const std::string packageName = "com.example.target";
    const std::string libraryName = "libsample.so";
    const std::string sourcePath = sourceTemplate;
    const std::string artifactId = "20260723-231440_libsample.so";
    const std::string manifest = "PACKAGE=com.example.target\nUID=11010\nSEGMENT_0_RANGE_0_FILE_OFFSET=0x100\n";
    const std::array<unsigned char, 4> before{{0xAA, 0xBB, 0xCC, 0xDD}};
    const std::array<unsigned char, 4> after{{0x11, 0x22, 0x33, 0x44}};
    CompanionArtifactHeader header;
    header.uid = 11010U;
    header.packageLength = packageName.size();
    header.libraryNameLength = libraryName.size();
    header.sourcePathLength = sourcePath.size();
    header.artifactIdLength = artifactId.size();
    header.manifestLength = manifest.size();
    header.segmentCount = 1U;
    header.totalSnapshotBytes = before.size();
    CompanionArtifactSegmentHeader segment;
    segment.memoryAddress = 0x7C9C062100ULL;
    segment.fileOffset = 0x100U;
    segment.size = before.size();
    Require(SendAll(sockets[0], &header, sizeof(header)) &&
            SendAll(sockets[0], packageName.data(), packageName.size()) &&
            SendAll(sockets[0], libraryName.data(), libraryName.size()) &&
            SendAll(sockets[0], sourcePath.data(), sourcePath.size()) &&
            SendAll(sockets[0], artifactId.data(), artifactId.size()) &&
            SendAll(sockets[0], manifest.data(), manifest.size()) &&
            SendAll(sockets[0], &segment, sizeof(segment)) &&
            SendAll(sockets[0], before.data(), before.size()) &&
            SendAll(sockets[0], after.data(), after.size()), "could not send artifact request");
    std::uint32_t response = 0;
    Require(ReceiveAll(sockets[0], &response, sizeof(response)) && response == 1U, "root companion rejected artifact");
    shutdown(sockets[0], SHUT_WR);
    close(sockets[0]);
    companion.join();

    const std::string base = std::string(root) + "/artifacts/com.example.target_11010/" + artifactId;
    const auto diskArtifact = ReadBytes(base + "/disk-original.so");
    const auto beforeArtifact = ReadBytes(base + "/before.so");
    const auto afterArtifact = ReadBytes(base + "/after.so");
    Require(diskArtifact == disk, "disk-original artifact must match source SO");
    Require(beforeArtifact.size() == disk.size() && afterArtifact.size() == disk.size(), "artifact SO sizes must match source");
    Require(std::memcmp(beforeArtifact.data() + 0x100, before.data(), before.size()) == 0, "before SO must contain pre-restore bytes");
    Require(std::memcmp(afterArtifact.data() + 0x100, after.data(), after.size()) == 0, "after SO must contain post-restore bytes");
    Require(ReadText(base + "/manifest.txt") == manifest, "manifest must retain exact target payload");
    const std::string ranges = ReadText(base + "/diff-ranges.txt");
    Require(ranges.find("FILE_OFFSET=0x100") != std::string::npos &&
                ranges.find("MEMORY_ADDRESS=0x7C9C062100") != std::string::npos &&
                ranges.find("BEFORE_PREVIEW=AA BB CC DD") != std::string::npos &&
                ranges.find("AFTER_PREVIEW=11 22 33 44") != std::string::npos,
            "diff range artifact must contain file and memory locations with before/after bytes");

    std::string cleanup = std::string("rm -rf '") + root + "' '" + sourceTemplate + "'";
    Require(std::system(cleanup.c_str()) == 0, "artifact fixture cleanup failed");
    std::puts("PASS: root companion materializes disk, before, after, and range artifacts");
    return 0;
}
