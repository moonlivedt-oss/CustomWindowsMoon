#pragma once
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <array>
#include <optional>
#include <atomic>
#include <cstdint>

namespace pm {
// Only an index for candidates of equal size: matches are always confirmed by SameFileContents.
// So the start and the end of the file are enough - a rescan does not reread whole albums.
inline std::optional<uint64_t> FileFingerprint(const std::filesystem::path& path, const std::atomic<bool>* cancel = nullptr) {
    constexpr std::streamoff kSample = 65536;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size < 0) return {};
    std::array<char, kSample> buffer;
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](std::streamoff from) {
        in.seekg(from);
        in.read(buffer.data(), static_cast<std::streamsize>(std::min(kSample, size - from)));
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buffer[(size_t)i]);
            hash *= 1099511628211ull;
        }
        return static_cast<bool>(in);
    };
    if (cancel && cancel->load()) return {};
    if (!mix(0)) return {};
    if (size > kSample && !mix(std::max(kSample, size - kSample))) return {};
    return hash;
}
inline bool SameFileContents(const std::filesystem::path& a, const std::filesystem::path& b, const std::atomic<bool>* cancel = nullptr) {
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) return false;
    std::array<char, 65536> x, y;
    do {
        if (cancel && cancel->load()) return false;
        left.read(x.data(), x.size()); right.read(y.data(), y.size());
        if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
    } while (left && right);
    return left.eof() && right.eof();
}
}
