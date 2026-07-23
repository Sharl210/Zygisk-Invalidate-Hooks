#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../module/src/main/cpp/main.cpp"

RestoreStats InspectOrRestoreLibrary(const RestoreConfig &) { return {}; }
const char *RestoreModeName(RestoreMode) { return "restore"; }
const char *RestoreOutcomeName(RestoreOutcome outcome) { return outcome == RestoreOutcome::kRestored ? "restored" : "failed"; }

namespace {
[[noreturn]] void Fail(const char *message) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
void Require(bool condition, const char *message) { if (!condition) Fail(message); }
std::string ReadFile(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "re");
    Require(file != nullptr, "could not open companion-created log");
    std::string result; char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) result += buffer;
    std::fclose(file); return result;
}
}  // namespace

int main() {
    char rootTemplate[] = "/tmp/inline-hook-spoof-companion-XXXXXX";
    char *root = mkdtemp(rootTemplate);
    Require(root != nullptr, "mkdtemp failed");
    const int rootFd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(rootFd >= 0, "could not open temporary module directory");
    int sockets[2] = {-1, -1};
    Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    std::thread companion([&] { CompanionHandler(sockets[1]); });
    Require(SendModuleDirFd(sockets[0], rootFd), "could not pass module directory FD to companion");
    close(rootFd);
    const std::string payload = "EVENT_TIME=2026-07-23T12:34:56\nRECOVERY_STAGE=preAppSpecialize_package_uid_root_companion\nEND_EVENT\n\n";
    std::string error;
    Require(SendCompanionLog(sockets[0], "com.example.target", 11010U, "20260723-123456.log", payload, &error),
            "synchronous pre companion log event must be accepted");
    shutdown(sockets[0], SHUT_WR); close(sockets[0]); companion.join();
    const std::string logPath = std::string(root) + "/logs/com.example.target_11010/20260723-123456.log";
    Require(ReadFile(logPath) == payload, "companion must persist exact package UID payload");
    std::string cleanup = std::string("rm -rf '") + root + "'";
    Require(std::system(cleanup.c_str()) == 0, "temporary companion cleanup failed");
    std::puts("PASS: synchronous pre companion writes package UID log without post-stage dependency");
    return 0;
}
