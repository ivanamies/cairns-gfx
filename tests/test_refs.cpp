// tests/test_refs.cpp
//
// Ref loader + auto-baker. First run on a platform: no ref file exists, so
// the observed hash is written to disk and returned (auto-bake) -- the
// REQUIRE then trivially passes for that run. Subsequent runs: read the
// file. Set CAIRNS_GFX_BAKE_REFS=1 to force re-baking (maintainers regen
// after intentional rendering changes).

#include "test_refs.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path RefsRoot() {
    // tests/refs/ relative to this TU's source path. CMake gives us the
    // source dir via CAIRNS_TEST_REFS_DIR; fall back to the workdir's
    // "tests/refs/" if not set.
    if (const char* env = std::getenv("CAIRNS_TEST_REFS_DIR")) {
        return std::filesystem::path(env);
    }
#ifdef CAIRNS_TEST_REFS_DIR_DEFINE
    return std::filesystem::path(CAIRNS_TEST_REFS_DIR_DEFINE);
#else
    return std::filesystem::path("tests/refs");
#endif
}

bool ForceBake() {
    if (const char* env = std::getenv("CAIRNS_GFX_BAKE_REFS")) {
        return env[0] && env[0] != '0';
    }
    return false;
}

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

bool WriteAll(const std::filesystem::path& p, const std::string& s) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p);
    if (!f) {
        return false;
    }
    f << s << "\n";
    return f.good();
}

std::string LoadOrBake(const std::filesystem::path& ref_path,
                       const std::string& observed) {
    if (!ForceBake() && std::filesystem::exists(ref_path)) {
        return ReadAll(ref_path);
    }
    if (WriteAll(ref_path, observed)) {
        return observed;
    }
    return {};
}

}  // namespace

namespace cairns::test_refs {

std::string LoadImageRef(const std::string& name, const std::string& platform,
                         const std::string& observed) {
    const auto path = RefsRoot() / (name + "." + platform + ".imghash");
    return LoadOrBake(path, observed);
}

std::string LoadSkinRef(const std::string& name, const std::string& observed) {
    const auto path = RefsRoot() / (name + ".skinhash");
    return LoadOrBake(path, observed);
}

}  // namespace cairns::test_refs
