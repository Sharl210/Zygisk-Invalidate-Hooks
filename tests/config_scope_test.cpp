#include <cstdio>
#include <cstdlib>
#include <string>

// Include implementation directly so internal configuration helpers can be
// exercised without widening the production native API.
#include "../module/src/main/cpp/main.cpp"

RestoreStats InspectOrRestoreLibrary(const RestoreConfig &) {
    return {};
}
const char *RestoreModeName(RestoreMode) {
    return "test";
}
const char *RestoreOutcomeName(RestoreOutcome) {
    return "test";
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

const AppRule *FindRuleForTest(const ModuleConfig &config, const std::string &packageName) {
    for (const AppRule &rule : config.rules) {
        if (rule.packageName == packageName) {
            return &rule;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    ModuleConfig legacy;
    std::string error;
    Require(ParseLegacyConfig("1:libcustom_target.so\ncom.example.target\n", &legacy, &error),
            "original enabled config must parse");
    Require(legacy.enabled && legacy.rules.size() == 1, "legacy enabled config must retain its rule");
    const AppRule *legacyRule = FindRuleForTest(legacy, "com.example.target");
    Require(legacyRule != nullptr && legacyRule->uid == 0U && legacyRule->libraries.size() == 1 &&
                legacyRule->libraries[0] == "libcustom_target.so",
            "legacy custom library must be preserved");
    Require(FindRuleForTest(legacy, "com.example.other") == nullptr,
            "unselected package must not match");
    Require(FindRuleForTest(legacy, "com.example.target:worker") == nullptr,
            "subprocess must not match without an explicit rule");

    ModuleConfig pathLibrary;
    error.clear();
    Require(ParseLegacyConfig("1:/apex/com.android.art/lib64/libart.so\ncom.example.target\n", &pathLibrary,
                              &error),
            "legacy path-style custom library must parse");
    Require(FindRuleForTest(pathLibrary, "com.example.target")->libraries[0] ==
                "/apex/com.android.art/lib64/libart.so",
            "path-style custom library must remain unchanged");

    ModuleConfig perApp;
    error.clear();
    Require(ParsePerAppConfig(
                "version=3\n"
                "enabled=true\n"
                "app=com.example.alpha|libart.so, libalpha.so，/apex/example/libbeta.so|true|11010\n"
                "app=com.example.alpha|libclone.so|false|21010\n"
                "app=com.example.beta||false|11011\n",
                &perApp, &error),
            "per-app multi-library config must parse");
    Require(perApp.enabled && perApp.rules.size() == 3, "per-app config must retain same-package clone rules");
    const AppRule *alpha = FindRuleForTest(perApp, "com.example.alpha");
    Require(alpha != nullptr && alpha->uid == 11010U && alpha->logEnabled && alpha->libraries.size() == 3 && alpha->libraries[0] == "libart.so" &&
                alpha->libraries[1] == "libalpha.so" && alpha->libraries[2] == "/apex/example/libbeta.so",
            "English and Chinese comma library separators must work");
    const AppRule *beta = FindRuleForTest(perApp, "com.example.beta");
    Require(beta != nullptr && beta->uid == 11011U && !beta->logEnabled && beta->libraries.size() == 1 && beta->libraries[0] == "libart.so",
            "an app without a custom list must receive the default libart.so");
    const AppRule *cloneAlpha = FindRuleByPackageAndUid(perApp, "com.example.alpha", 21010U);
    Require(cloneAlpha != nullptr && cloneAlpha->libraries.size() == 1 && cloneAlpha->libraries[0] == "libclone.so",
            "same-package clone must retain an independent UID rule");
    Require(FindRuleByPackageAndUid(perApp, "com.example.alpha", 11010U) == alpha &&
                FindRuleByPackageAndUid(perApp, "com.example.beta", 11011U) == beta &&
                FindRuleByPackageAndUid(perApp, "com.example.alpha", 99999U) == nullptr,
            "UID matching must target only the configured application rule");
    Require(FindRuleForTest(perApp, "com.example.alpha:worker") == nullptr,
            "per-app rules must be exact and isolate child processes");

    ModuleConfig emptySelection;
    error.clear();
    Require(ParsePerAppConfig("version=3\nenabled=true\n", &emptySelection, &error),
            "enabled config without app rows must represent a clean empty selection");
    Require(emptySelection.enabled && emptySelection.rules.empty() &&
                FindRuleForTest(emptySelection, "com.example.unconfigured") == nullptr,
            "empty selection must not match any application");

    ModuleConfig disabled;
    error.clear();
    Require(ParseLegacyConfig("0:libart.so\n", &disabled, &error),
            "disabled original config without package rows must parse");
    Require(!disabled.enabled && disabled.rules.empty(),
            "disabled config must keep a clean no-target state");

    std::puts("PASS: legacy compatibility, per-app multi-library rules, exact scope, default library, and cancellation semantics");
    return 0;
}
