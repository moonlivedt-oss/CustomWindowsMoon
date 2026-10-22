#include "gfx/qr.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>

namespace qr {

namespace {

// коррекция M, версии 1-10: кодовых слов коррекции в блоке и число блоков
constexpr int kEccPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kBlocks[11] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int kMaxVersion = 10;

// модулей под данные и коррекцию (без служебных узоров)
int RawModules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int n = ver / 7 + 2;
        r -= (25 * n - 10) * n - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}

int DataCodewords(int ver) { return RawModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

std::vector<int> AlignPositions(int ver) {
    if (ver == 1) return {};
    int n = ver / 7 + 2, step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    std::vector<int> out;
    for (int i = 0, pos = ver * 4 + 10; i < n - 1; ++i, pos -= step) out.insert(out.begin(), pos);
    out.insert(out.begin(), 6);
    return out;
}

// ---- Рид-Соломон над GF(256), многочлен 0x11D ----
uint8_t Mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

std::vector<uint8_t> Divisor(int degree) {
    std::vector<uint8_t> r(degree, 0);
    r[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < r.size(); ++j) {
            r[j] = Mul(r[j], root);
            if (j + 1 < r.size()) r[j] ^= r[j + 1];
        }
        root = Mul(root, 0x02);
    }
    return r;
}

std::vector<uint8_t> Remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); ++i) r[i] ^= Mul(div[i], f);
    }
    return r;
}

struct Grid {
    int n;
    std::vector<bool> dark, fixed;  // fixed - служебный модуль, данные и маска его не трогают
    explicit Grid(int size) : n(size), dark((size_t)size * size), fixed((size_t)size * size) {}
    void Set(int x, int y, bool d) {
        dark[(size_t)y * n + x] = d;
        fixed[(size_t)y * n + x] = true;
    }
    bool Dark(int x, int y) const { return dark[(size_t)y * n + x]; }
};

void Finder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            int x = cx + dx, y = cy + dy, d = std::max(std::abs(dx), std::abs(dy));
            if (x >= 0 && x < g.n && y >= 0 && y < g.n) g.Set(x, y, d != 2 && d != 4);
        }
}

void FormatBits(Grid& g, int mask) {
    int data = (0 << 3) | mask;  // коррекция M - 00
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) g.Set(8, i, bit(i));
    g.Set(8, 7, bit(6));
    g.Set(8, 8, bit(7));
    g.Set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.Set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) g.Set(g.n - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.Set(8, g.n - 15 + i, bit(i));
    g.Set(8, g.n - 8, true);  // всегда тёмный модуль
}

void Functions(Grid& g, int ver) {
    for (int i = 0; i < g.n; ++i) {
        g.Set(6, i, i % 2 == 0);
        g.Set(i, 6, i % 2 == 0);
    }
    Finder(g, 3, 3);
    Finder(g, g.n - 4, 3);
    Finder(g, 3, g.n - 4);
    std::vector<int> al = AlignPositions(ver);
    int last = (int)al.size() - 1;
    for (int i = 0; i <= last; ++i)
        for (int j = 0; j <= last; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0)) continue;  // там искатели
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    g.Set(al[i] + dx, al[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    FormatBits(g, 0);  // место под формат; настоящие биты - после выбора маски
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = (long)ver << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            bool b = ((bits >> i) & 1) != 0;
            int a = g.n - 11 + i % 3, c = i / 3;
            g.Set(a, c, b);
            g.Set(c, a, b);
        }
    }
}

bool MaskBit(int mask, int x, int y) {
    switch (mask) {
        case 0:
            return (x + y) % 2 == 0;
        case 1:
            return y % 2 == 0;
        case 2:
            return x % 3 == 0;
        case 3:
            return (x + y) % 3 == 0;
        case 4:
            return (x / 3 + y / 2) % 2 == 0;
        case 5:
            return x * y % 2 + x * y % 3 == 0;
        case 6:
            return (x * y % 2 + x * y % 3) % 2 == 0;
        default:
            return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void ApplyMask(Grid& g, int mask) {
    for (int y = 0; y < g.n; ++y)
        for (int x = 0; x < g.n; ++x)
            if (!g.fixed[(size_t)y * g.n + x] && MaskBit(mask, x, y)) g.dark[(size_t)y * g.n + x].flip();
}

// Штраф маски (правила 1, 2 и 4 спецификации; правило 3 - похожие на искатель узоры - для выбора не нужно).
long Penalty(const Grid& g) {
    long p = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < g.n; ++a) {
            int run = 1;
            for (int b = 1; b < g.n; ++b) {
                bool cur = pass ? g.Dark(a, b) : g.Dark(b, a), prev = pass ? g.Dark(a, b - 1) : g.Dark(b - 1, a);
                if (cur == prev) {
                    if (++run == 5)
                        p += 3;
                    else if (run > 5)
                        ++p;
                } else {
                    run = 1;
                }
            }
        }
    for (int y = 0; y + 1 < g.n; ++y)
        for (int x = 0; x + 1 < g.n; ++x) {
            bool c = g.Dark(x, y);
            if (c == g.Dark(x + 1, y) && c == g.Dark(x, y + 1) && c == g.Dark(x + 1, y + 1)) p += 3;
        }
    long darkCount = std::count(g.dark.begin(), g.dark.end(), true), total = (long)g.n * g.n;
    long k = (std::labs(darkCount * 20 - total * 10) + total - 1) / total - 1;
    return p + std::max(0L, k) * 10;
}

}  // namespace

Code Encode(const std::string& bytes) {
    Code out;
    int ver = 1;
    auto bitsNeeded = [&](int v) { return 4 + (v < 10 ? 8 : 16) + 8 * (int)bytes.size(); };
    while (ver <= kMaxVersion && bitsNeeded(ver) > DataCodewords(ver) * 8) ++ver;
    if (ver > kMaxVersion) return out;

    // биты: режим «байты», длина, данные, конец, добивка до байта и чередование 0xEC 0x11
    std::vector<bool> bits;
    auto put = [&](unsigned v, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(((v >> i) & 1) != 0);
    };
    put(4, 4);
    put((unsigned)bytes.size(), ver < 10 ? 8 : 16);
    for (unsigned char c : bytes) put(c, 8);
    int cap = DataCodewords(ver) * 8;
    put(0, std::min(4, cap - (int)bits.size()));
    put(0, (8 - (int)bits.size() % 8) % 8);
    for (bool flip = false; (int)bits.size() < cap; flip = !flip) put(flip ? 0x11 : 0xEC, 8);
    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) data[i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));

    // блоки с коррекцией и перемежение
    int blocks = kBlocks[ver], eccLen = kEccPerBlock[ver], raw = RawModules(ver) / 8;
    int shortBlocks = blocks - raw % blocks, shortLen = raw / blocks;
    std::vector<uint8_t> div = Divisor(eccLen);
    std::vector<std::vector<uint8_t>> parts;
    for (int i = 0, k = 0; i < blocks; ++i) {
        int len = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + len);
        k += len;
        std::vector<uint8_t> ecc = Remainder(dat, div);
        if (i < shortBlocks) dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        parts.push_back(std::move(dat));
    }
    std::vector<uint8_t> words;
    for (size_t i = 0; i < parts[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != (size_t)(shortLen - eccLen) || j >= shortBlocks) words.push_back(parts[j][i]);

    // раскладка змейкой по парам столбцов снизу вверх и обратно
    int size = ver * 4 + 17;
    Grid g(size);
    Functions(g, ver);
    size_t bit = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;  // столбец синхронизации
        for (int vert = 0; vert < size; ++vert)
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                bool up = ((right + 1) & 2) == 0;
                int y = up ? size - 1 - vert : vert;
                if (g.fixed[(size_t)y * size + x] || bit >= words.size() * 8) continue;
                g.dark[(size_t)y * size + x] = ((words[bit >> 3] >> (7 - (bit & 7))) & 1) != 0;
                ++bit;
            }
    }

    // маска с наименьшим штрафом
    long best = LONG_MAX;
    int bestMask = 0;
    for (int m = 0; m < 8; ++m) {
        Grid t = g;
        ApplyMask(t, m);
        FormatBits(t, m);
        long p = Penalty(t);
        if (p < best) best = p, bestMask = m;
    }
    ApplyMask(g, bestMask);
    FormatBits(g, bestMask);
    out.size = size;
    out.version = ver;
    out.mask = bestMask;
    out.dark = std::move(g.dark);
    return out;
}

std::string WifiText(const std::string& ssid, const std::string& password, bool open) {
    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') r += '\\';
            r += c;
        }
        return r;
    };
    return "WIFI:T:" + std::string(open ? "nopass" : "WPA") + ";S:" + esc(ssid) + ";" +
           (open ? std::string() : "P:" + esc(password) + ";") + ";";
}

}  // namespace qr
