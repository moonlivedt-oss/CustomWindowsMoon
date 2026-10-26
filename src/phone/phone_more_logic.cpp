// «Телефон рядом», второй заход: ZIP на лету без сжатия, пути в папках ПК, загрузка кусками, скорость (под тестами).
#include <cmath>

#include "core/text.h"
#include "phone/phone.h"

namespace phone {

namespace {

void Put16(std::string& s, unsigned v) {
    s += (char)(v & 255);
    s += (char)((v >> 8) & 255);
}

void Put32(std::string& s, uint32_t v) {
    Put16(s, v & 0xFFFF);
    Put16(s, v >> 16);
}

}  // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

// Без сжатия (store), имена в UTF-8 (бит 11), CRC - в дескрипторе после данных (бит 3): размеры известны заранее,
// поэтому и длина всего архива известна до отправки.
std::string ZipLocal(const std::string& name, uint32_t size, uint16_t dosTime, uint16_t dosDate) {
    std::string s;
    Put32(s, 0x04034b50);
    Put16(s, 20);
    Put16(s, (1u << 3) | (1u << 11));
    Put16(s, 0);  // store
    Put16(s, dosTime);
    Put16(s, dosDate);
    Put32(s, 0);  // crc - в дескрипторе
    Put32(s, size);
    Put32(s, size);
    Put16(s, (unsigned)name.size());
    Put16(s, 0);
    return s + name;
}

std::string ZipDescriptor(uint32_t crc, uint32_t size) {
    std::string s;
    Put32(s, 0x08074b50);
    Put32(s, crc);
    Put32(s, size);
    Put32(s, size);
    return s;
}

std::string ZipCentral(const ZipEntry& e) {
    std::string s;
    Put32(s, 0x02014b50);
    Put16(s, 20);
    Put16(s, 20);
    Put16(s, (1u << 3) | (1u << 11));
    Put16(s, 0);
    Put16(s, e.dosTime);
    Put16(s, e.dosDate);
    Put32(s, e.crc);
    Put32(s, e.size);
    Put32(s, e.size);
    Put16(s, (unsigned)e.name.size());
    Put16(s, 0);
    Put16(s, 0);
    Put16(s, 0);
    Put16(s, 0);
    Put32(s, 0);
    Put32(s, e.offset);
    return s + e.name;
}

std::string ZipEnd(size_t count, uint32_t dirSize, uint32_t dirOffset) {
    std::string s;
    Put32(s, 0x06054b50);
    Put16(s, 0);
    Put16(s, 0);
    Put16(s, (unsigned)count);
    Put16(s, (unsigned)count);
    Put32(s, dirSize);
    Put32(s, dirOffset);
    Put16(s, 0);
    return s;
}

long long ZipSize(const std::vector<std::pair<std::string, long long>>& files) {
    long long n = 22;
    for (const auto& [name, size] : files) n += 30 + (long long)name.size() + size + 16 + 46 + (long long)name.size();
    return n;
}

bool ZipFits(const std::vector<std::pair<std::string, long long>>& files) {
    return files.size() < 65535 && ZipSize(files) < 0xFFFFFFFFll;
}

std::string ZipName(const std::wstring& rel) {
    std::wstring r = rel;
    for (wchar_t& c : r)
        if (c == L'\\') c = L'/';
    while (!r.empty() && r[0] == L'/') r.erase(0, 1);
    return Narrow(r);
}

std::wstring SafeRel(const std::string& utf8) {
    std::wstring p = Widen(utf8), out;
    if (p.size() > 1000) return L"?";
    for (const std::wstring& part : Split(p, L'/')) {
        if (part.empty()) continue;
        if (part == L"." || part == L".." || part.find_first_of(L"\\:*?\"<>|") != std::wstring::npos) return L"?";
        for (wchar_t c : part)
            if (c < 32) return L"?";
        out += (out.empty() ? L"" : L"\\") + part;
    }
    return out;
}

bool UploadId(const std::string& id) {
    if (id.size() < 8 || id.size() > 40) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

std::wstring RateText(const wchar_t* verb, double bytesPerSec, long long left) {
    if (bytesPerSec < 1024) return L"";
    wchar_t b[96];
    if (bytesPerSec >= 1048576)
        swprintf(b, 96, L"%ls %.1f МБ/с", verb, bytesPerSec / 1048576.0);
    else
        swprintf(b, 96, L"%ls %.0f КБ/с", verb, bytesPerSec / 1024.0);
    std::wstring s = b;
    for (wchar_t& c : s)
        if (c == L'.') c = L',';
    if (left > 0) {
        long long sec = (long long)std::ceil(left / bytesPerSec);
        s += sec < 60 ? L", ещё " + std::to_wstring(sec) + L" с"
                      : L", ещё " + std::to_wstring(sec / 60) + L" мин " + std::to_wstring(sec % 60) + L" с";
    }
    return s;
}

}  // namespace phone
