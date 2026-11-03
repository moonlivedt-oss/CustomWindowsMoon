// Source-level coverage: a misspelled Hit key or an unmapped config switch fails radial_tests.
#include "core/usage_features.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

namespace {
std::string ReadSource(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::wstring WideKey(const std::string& s) { return {s.begin(), s.end()}; }
}

void TestUsageCatalog(void (*check)(bool, const char*, int)) {
#define CHECK_USAGE(x) check((x), #x, __LINE__)
    std::set<std::wstring> keys, hitKeys;
    std::vector<bool Config::*> flags;
    for (const auto& f : usage::kFeatures) {
        CHECK_USAGE(f.key && *f.key && f.label && *f.label && f.flag);
        CHECK_USAGE(keys.insert(f.key).second);
        CHECK_USAGE(std::find(flags.begin(), flags.end(), f.flag) == flags.end());
        flags.push_back(f.flag);
    }
    CHECK_USAGE(keys.count(L"palette") == 1);
    CHECK_USAGE(keys.count(L"pallete") == 0);  // regression: a typo must fail membership
    const std::filesystem::path root = std::filesystem::u8path(RADIAL_SOURCE_DIR);
    const std::regex call(R"(usage\s*::\s*Hit\s*\()"), literal(R"re(usage\s*::\s*Hit\s*\(\s*L"([a-z0-9_]+)"\s*\))re");
    size_t calls = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root / "src")) {
        if (!e.is_regular_file() || (e.path().extension() != ".cpp" && e.path().extension() != ".h")) continue;
        const auto s = ReadSource(e.path());
        const auto n = std::distance(std::sregex_iterator(s.begin(), s.end(), call), std::sregex_iterator());
        const auto literals = std::distance(std::sregex_iterator(s.begin(), s.end(), literal), std::sregex_iterator());
        CHECK_USAGE(n == literals);  // dynamic expressions bypass key validation: keep call sites literal
        calls += static_cast<size_t>(n);
        for (auto it = std::sregex_iterator(s.begin(), s.end(), literal); it != std::sregex_iterator(); ++it) {
            auto key = WideKey((*it)[1].str());
            if (!keys.count(key)) std::cerr << "Unmapped usage key in " << e.path() << ": " << (*it)[1] << '\n';
            CHECK_USAGE(keys.count(key) == 1);
            hitKeys.insert(key);
        }
    }
    CHECK_USAGE(calls >= keys.size());
    for (const auto& key : keys) {
        if (!hitKeys.count(key)) std::wcerr << L"Feature has no usage event: " << key << L'\n';
        CHECK_USAGE(hitKeys.count(key) == 1);
    }
    const auto config = ReadSource(root / "src/core/config.cpp");
    const auto catalog = ReadSource(root / "src/core/usage_features.h");
    CHECK_USAGE(!config.empty() && !catalog.empty());
    const std::regex configFlag(R"(cfg\.(\w+)\s*=\s*ParseBool\(val\))"), catalogFlag(R"(&Config::(\w+))");
    std::set<std::string> mapped, switches;
    for (auto it = std::sregex_iterator(catalog.begin(), catalog.end(), catalogFlag); it != std::sregex_iterator(); ++it)
        mapped.insert((*it)[1]);
    for (auto it = std::sregex_iterator(config.begin(), config.end(), configFlag); it != std::sregex_iterator(); ++it)
        if ((*it)[1] != "tourDone") switches.insert((*it)[1]);
    // These boolean UI choices are serialized as text. tourDone is onboarding state.
    switches.insert("accentAuto");
    switches.insert("lockPortal");
    CHECK_USAGE(mapped == switches);
    CHECK_USAGE(mapped.size() == flags.size());
    // Hit must remain memory-only, including the very first call.
    const auto runtime = ReadSource(root / "src/core/usage.cpp");
    auto begin = runtime.find("void Hit("), end = runtime.find("void Flush(");
    CHECK_USAGE(begin != std::string::npos && end != std::string::npos && begin < end);
    if (begin != std::string::npos && end != std::string::npos && begin < end) {
        const auto body = runtime.substr(begin, end - begin);
        CHECK_USAGE(body.find("Read();") == std::string::npos && body.find("Flush(") == std::string::npos);
    }
#undef CHECK_USAGE
}
