#include "helpers/ocrtext_logic.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ocrtext {

namespace {

bool IsLatin(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
}
bool IsCyril(wchar_t c) { return c >= 0x400 && c <= 0x52F; }
bool IsCjk(wchar_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF);
}
bool IsLetter(wchar_t c) { return IsLatin(c) || IsCyril(c) || IsCjk(c); }
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsUpper(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= 0xC0 && c <= 0xDE) || (c >= 0x400 && c <= 0x42F); }
bool IsLower(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= 0xDF && c <= 0xFF) || (c >= 0x430 && c <= 0x45F); }
bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xA0; }

std::wstring TrimText(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (IsSpace(c)) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// имя файла, адрес, почта: не слово, но и не мусор
bool FileOrUrl(const std::wstring& w) {
    if (w.find_first_of(L"/_@") != std::wstring::npos || w.find(L"://") != std::wstring::npos) return true;
    size_t dot = w.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 == w.size() || w.size() - dot - 1 > 5) return false;
    for (size_t i = dot + 1; i < w.size(); ++i)
        if (!(w[i] < 128 && (IsLetter(w[i]) || IsDigit(w[i])))) return false;
    return true;
}

// 1 - похоже на настоящее слово, 0 - нет, -1 - не слово вовсе (число, знак, файл, адрес), не считается
// strict - заглавная посреди слова тоже подозрительна: так выглядит чужой алфавит, но и «GitHub», поэтому только
// для сравнения двух прочтений одной строки
int TokenOk(const std::wstring& tok, bool strict = false) {
    static const wchar_t kEdge[] = L"\"'«»“”„()[]{}.,!?:;…-–—*/\\|<>";
    size_t a = tok.find_first_not_of(kEdge), b = tok.find_last_not_of(kEdge);
    if (a == std::wstring::npos) return -1;
    std::wstring word = tok.substr(a, b - a + 1);
    std::wstring letters;
    bool digit = false, cyril = false, latin = false, cjk = false, odd = false;
    for (wchar_t c : word) {
        if (IsLetter(c)) letters += c;
        digit = digit || IsDigit(c);
        cyril = cyril || IsCyril(c);
        latin = latin || IsLatin(c);
        cjk = cjk || IsCjk(c);
        odd = odd || wcschr(L"«»@#$%^&*=~`|\\<>{}[]", c);
    }
    if (letters.empty() || FileOrUrl(word)) return -1;
    if (cjk) return 1;
    if (digit || odd || (latin && cyril)) return 0;  // «pa60Tana»: кириллица, прочитанная латиницей
    if (letters.size() >= 4 &&
        letters.find_first_of(L"aeiouyAEIOUYаеёиоуыэюяАЕЁИОУЫЭЮЯіїєІЇЄàáâäèéêëìíîïòóôöùúûü") == std::wstring::npos)
        return 0;
    // регистр скачет внутри слова: две заглавные подряд и строчные рядом («CMeHa»; «iPhone» и «OK» - можно)
    bool lower = false, pair = false;
    for (size_t i = 0; i < letters.size(); ++i) {
        lower = lower || IsLower(letters[i]);
        pair = pair || (i + 1 < letters.size() && IsUpper(letters[i]) && IsUpper(letters[i + 1]));
    }
    if (pair && lower) return 0;
    if (strict)
        for (size_t i = 1; i < letters.size(); ++i)
            if (IsUpper(letters[i]) && IsLower(letters[i - 1])) return 0;
    return 1;
}

double Quality(const std::wstring& text, bool strict) {
    int good = 0, total = 0;
    for (const std::wstring& tok : Tokens(text)) {
        int ok = TokenOk(tok, strict);
        if (ok < 0) continue;
        int n = (int)std::count_if(tok.begin(), tok.end(), IsLetter);
        total += n;
        good += ok ? n : 0;
    }
    return total ? (double)good / total : 1.0;
}

int Letters(const std::wstring& s) { return (int)std::count_if(s.begin(), s.end(), IsLetter); }

// Точно русская строка: есть кириллические буквы, которых нет среди похожих на латиницу
bool SurelyCyrillic(const std::wstring& s) {
    static const wchar_t lookalike[] = L"авекмнопрстухьАВЕКМНОПРСТУХЬ";
    for (wchar_t c : s)
        if (IsCyril(c) && !wcschr(lookalike, c)) return true;
    return false;
}

// Какое из прочтений одной строки брать: английского движка или основного (русского)
bool PreferEnglish(const std::wstring& main, const std::wstring& alt) {
    if (alt.empty()) return false;
    if (!SurelyCyrillic(main)) return TextQuality(alt) >= TextQuality(main);
    // русские буквы есть, но это может быть английская строка, где основной ошибся в паре слов: «Free ир some space оп»
    int cyr = (int)std::count_if(main.begin(), main.end(), IsCyril),
        lat = (int)std::count_if(main.begin(), main.end(), IsLatin);
    const double strictMain = Quality(main, true), strictAlt = Quality(alt, true);
    if (lat >= cyr) return strictAlt >= strictMain;
    // вся строка кириллицей, но слова рваные («ВасКир Мападег»), а английский прочёл чисто
    return strictAlt > strictMain + 0.2;
}

float Median(std::vector<float> v) {
    if (v.empty()) return 1;
    std::sort(v.begin(), v.end());
    return std::max(1.0f, v[v.size() / 2]);
}

float Overlap(const Line& a, const Line& b) {  // доля меньшей строки, которую накрывает другая
    float w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x),
          h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    if (w <= 0 || h <= 0) return 0;
    float small = std::min(a.w * a.h, b.w * b.h);
    return small > 0 ? w * h / small : 0;
}

bool IsBullet(const std::wstring& t) {
    if (t.empty()) return false;
    if (wcschr(L"-–—•·*▪►", t[0])) return t.size() > 1 && t[1] == L' ';
    size_t i = 0;
    while (i < t.size() && i < 3 && IsDigit(t[i])) ++i;
    return i > 0 && i + 1 < t.size() && (t[i] == L'.' || t[i] == L')') && t[i + 1] == L' ';
}

bool EndsWithAny(const std::wstring& t, const wchar_t* set) { return !t.empty() && wcschr(set, t.back()); }

// Соединить две строки одного абзаца: перенос по слогам («перево-» + «дчик») - без дефиса и пробела
void Glue(std::wstring& text, const std::wstring& next) {
    if (text.size() > 2 && text.back() == L'-' && IsLetter(text[text.size() - 2]) && !next.empty() &&
        IsLower(next[0])) {
        text.pop_back();
        text += next;
    } else {
        text += L' ';
        text += next;
    }
}

bool BlockIsCode(const std::vector<Line>& lines) {
    int code = 0;
    for (const Line& l : lines) code += LooksLikeCode(l.text);
    return code > 0 && code * 4 >= (int)lines.size();
}

void Span(const std::vector<Line>& lines, float& x0, float& x1) {
    x0 = lines[0].x;
    x1 = lines[0].x + lines[0].w;
    for (const Line& l : lines) {
        x0 = std::min(x0, l.x);
        x1 = std::max(x1, l.x + l.w);
    }
}

int BlockAlign(const std::vector<Line>& lines) {
    if (lines.size() < 2) return 0;
    std::vector<float> hs;
    float l0 = lines[0].x, l1 = l0, r0 = lines[0].x + lines[0].w, r1 = r0, m0 = lines[0].x + lines[0].w / 2, m1 = m0;
    for (const Line& l : lines) {
        hs.push_back(l.h);
        l0 = std::min(l0, l.x);
        l1 = std::max(l1, l.x);
        r0 = std::min(r0, l.x + l.w);
        r1 = std::max(r1, l.x + l.w);
        m0 = std::min(m0, l.x + l.w / 2);
        m1 = std::max(m1, l.x + l.w / 2);
    }
    float tol = 0.6f * Median(hs);
    if (l1 - l0 <= tol) return 0;  // и у выключки по ширине левые края ровные
    if (m1 - m0 <= tol) return 1;
    if (r1 - r0 <= tol) return 2;
    return 0;
}

}  // namespace

double TextQuality(const std::wstring& text) { return Quality(text, false); }

bool IsGarbageLine(const std::wstring& text) {
    return Tokens(text).size() >= 2 && Letters(text) >= 6 && TextQuality(text) < 0.25;
}

bool IsGarbageBlock(const std::wstring& text) { return Letters(text) >= 6 && TextQuality(text) < 0.4; }

bool IsNoise(const std::wstring& text) {
    for (wchar_t c : text)
        if (IsCjk(c)) return false;  // один иероглиф - уже слово
    return Letters(text) < 2;
}

bool LooksLikeCode(const std::wstring& text) {
    std::wstring t = TrimText(text);
    if (t.empty()) return false;
    if (t[0] == L'#' || t[0] == L'$' || EndsWithAny(t, L"{}([];\\")) return true;
    for (const wchar_t* m :
         {L"::", L"->", L"=>", L"==", L"!=", L"//", L"</", L"/>", L" = ", L"()", L"&&", L"||", L":\\", L"> "})
        if (t.find(m) != std::wstring::npos) return true;
    return false;
}

std::vector<Line> MergeEngines(const std::vector<Line>& main, const std::vector<Line>& en) {
    std::vector<Line> out = main;
    std::vector<bool> used(en.size(), false);
    for (Line& line : out) {
        int best = -1;
        float bestAt = 0.5f;
        for (size_t i = 0; i < en.size(); ++i) {
            if (used[i]) continue;
            float o = Overlap(line, en[i]);
            if (o > bestAt) {
                bestAt = o;
                best = (int)i;
            }
        }
        if (best < 0) continue;
        used[(size_t)best] = true;
        const std::wstring& alt = en[(size_t)best].text;
        if (PreferEnglish(line.text, alt)) line = en[(size_t)best];
    }
    // основной не увидел строку совсем: ставим по месту сверху вниз, порядок остальных не трогаем
    for (size_t i = 0; i < en.size(); ++i) {
        if (used[i] || en[i].text.empty()) continue;
        bool covered = false;
        for (const Line& l : out) covered = covered || Overlap(l, en[i]) > 0.3f;
        if (covered) continue;
        auto at = std::find_if(out.begin(), out.end(), [&](const Line& l) { return l.y > en[i].y; });
        out.insert(at, en[i]);
    }
    return out;
}

std::vector<std::vector<Line>> GroupBlocks(const std::vector<Line>& input) {
    std::vector<Line> lines;
    for (const Line& l : input)
        if (!TrimText(l.text).empty()) lines.push_back(l);
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Line& a, const Line& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<std::vector<Line>> blocks;
    for (const Line& ln : lines) {
        std::vector<Line>* target = nullptr;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            const Line& last = it->back();
            float h = std::max(1.0f, std::min(last.h, ln.h)), gap = ln.y - (last.y + last.h);
            if (gap > 1.0f * h || gap < -0.6f * h) continue;
            if (std::fabs(ln.h - last.h) > 0.45f * std::max(last.h, ln.h)) continue;
            float x0, x1;
            Span(*it, x0, x1);
            float overlap = std::min(x1, ln.x + ln.w) - std::max(x0, ln.x);
            if (overlap < 0.25f * std::min(ln.w, x1 - x0) && std::fabs(ln.x - x0) > 1.5f * h) continue;
            target = &*it;
            break;
        }
        if (target)
            target->push_back(ln);
        else
            blocks.push_back({ln});
    }
    return blocks;
}

// Весь снимок - таблица: строки делятся на ряды с одним числом ячеек, столбцы стоят ровно. Движок отдаёт такое по
// столбцам вперемешку; собираем по рядам, ячейки через табуляцию (вставится в таблицу как таблица)
bool GridText(const std::vector<Line>& lines, std::wstring& out) {
    if (lines.size() < 4) return false;
    for (const Line& l : lines)
        if (Tokens(l.text).size() > 5) return false;  // ячейка не бывает длинной фразой: это соседние столбцы текста
    std::vector<Line> sorted = lines;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Line& a, const Line& b) { return a.y < b.y; });
    std::vector<std::vector<Line>> rows;
    for (const Line& l : sorted) {
        if (!rows.empty()) {
            const Line& first = rows.back()[0];
            float over = std::min(first.y + first.h, l.y + l.h) - std::max(first.y, l.y);
            if (over > 0.5f * std::min(first.h, l.h)) {
                rows.back().push_back(l);
                continue;
            }
        }
        rows.push_back({l});
    }
    if (rows.size() < 2) return false;
    size_t cols = rows[0].size();
    // одна пропущенная ячейка не страшна, но ряд из одной и лишние столбцы - уже не таблица
    if (cols < 2) {
        for (const auto& r : rows) cols = std::max(cols, r.size());
        if (cols < 2) return false;
    }
    for (auto& r : rows) {
        if (r.size() < 2 || r.size() > cols) return false;
        std::stable_sort(r.begin(), r.end(), [](const Line& a, const Line& b) { return a.x < b.x; });
    }
    std::vector<float> hs;
    for (const Line& l : lines) hs.push_back(l.h);
    const float tol = 1.5f * Median(hs);
    std::vector<float> cx;
    for (const auto& r : rows)
        if (r.size() == cols && cx.empty())
            for (const Line& l : r) cx.push_back(l.x);
    if (cx.empty()) return false;
    for (const auto& r : rows)
        for (const Line& l : r) {
            bool hit = false;
            for (float x : cx) hit = hit || std::fabs(l.x - x) <= tol;
            if (!hit) return false;
        }
    out.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) out += L"\r\n";
        size_t cell = 0;
        for (size_t c = 0; c < cols; ++c) {
            if (c) out += L'\t';
            if (cell < rows[i].size() && std::fabs(rows[i][cell].x - cx[c]) <= tol) out += rows[i][cell++].text;
        }
    }
    return true;
}

std::wstring CleanText(const std::vector<Line>& input) {
    std::vector<Line> lines;
    for (const Line& l : input) {
        Line c = l;
        c.text = TrimText(c.text);
        if (!c.text.empty() && !IsGarbageLine(c.text)) lines.push_back(std::move(c));
    }
    std::wstring grid;
    if (GridText(lines, grid)) return grid;
    // В блоке, где есть перенесённый абзац, строки идут вместе: движок отдаёт соседние столбцы вперемешку, строка через
    // строку. Остальное остаётся в порядке движка: в форме подпись и значение должны идти рядом.
    std::vector<std::vector<Line>> blocks = GroupBlocks(lines);
    std::vector<std::wstring> text(blocks.size());
    std::vector<bool> prose(blocks.size(), false);
    std::vector<int> blockOf(lines.size(), -1);
    for (size_t b = 0; b < blocks.size(); ++b) {
        const std::vector<Line>& group = blocks[b];
        float x0, x1;
        Span(group, x0, x1);
        std::vector<float> hs;
        for (const Line& l : group) hs.push_back(l.h);
        const float medH = Median(hs), width = std::max(1.0f, x1 - x0);
        const bool code = BlockIsCode(group);
        for (size_t i = 0; i < group.size(); ++i) {
            const Line& l = group[i];
            if (i == 0) {
                text[b] = l.text;
            } else {
                const Line& p = group[i - 1];
                const float gap = l.y - (p.y + p.h);
                const bool join = !code && !IsBullet(l.text) && Tokens(p.text).size() >= 3 && p.w >= 14 * medH &&
                                  p.x + p.w - x0 >= 0.8f * width && std::fabs(l.x - p.x) <= 0.6f * medH &&
                                  gap <= 0.9f * medH && gap >= -0.6f * medH && std::fabs(l.h - p.h) <= 0.6f * medH &&
                                  (IsLower(l.text[0]) || EndsWithAny(p.text, L",-"));
                if (join) {
                    Glue(text[b], l.text);
                    prose[b] = true;
                } else {
                    text[b] += L"\r\n";
                    text[b] += l.text;
                }
            }
            for (size_t k = 0; k < lines.size(); ++k)
                if (blockOf[k] < 0 && lines[k].x == l.x && lines[k].y == l.y && lines[k].text == l.text) {
                    blockOf[k] = (int)b;
                    break;
                }
        }
    }
    std::wstring out;
    std::vector<bool> emitted(blocks.size(), false);
    auto add = [&](const std::wstring& piece) {
        if (!out.empty()) out += L"\r\n";
        out += piece;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const int b = blockOf[i];
        if (b < 0 || !prose[(size_t)b]) {
            add(lines[i].text);
        } else if (!emitted[(size_t)b]) {
            add(text[(size_t)b]);
            emitted[(size_t)b] = true;
        }
    }
    return out;
}

std::vector<Block> MakeBlocks(const std::vector<Line>& lines) {
    std::vector<Block> out;
    for (const std::vector<Line>& group : GroupBlocks(lines)) {
        Block b;
        float x0, x1;
        Span(group, x0, x1);
        float y0 = group[0].y, y1 = group[0].y + group[0].h;
        std::vector<float> hs;
        for (const Line& l : group) {
            y0 = std::min(y0, l.y);
            y1 = std::max(y1, l.y + l.h);
            hs.push_back(l.h);
        }
        b.x = x0;
        b.y = y0;
        b.w = x1 - x0;
        b.h = y1 - y0;
        b.lineH = Median(hs);
        b.align = BlockAlign(group);
        b.code = BlockIsCode(group);
        b.lines = (int)group.size();
        const float full = std::max(1.0f, x1 - x0);
        for (size_t i = 0; i < group.size(); ++i) {
            const std::wstring t = TrimText(group[i].text);
            if (i == 0) {
                b.text = t;
                continue;
            }
            const Line& p = group[i - 1];
            const bool shortPrev = p.x + p.w - x0 < 0.7f * full;
            // новая фраза с заглавной в коротких строках - это список, а не перенос: «Open settings», «Check disk»
            const bool continues =
                !t.empty() && (IsLower(t[0]) || EndsWithAny(p.text, L",-") || (!shortPrev && p.w >= 25 * b.lineH));
            const bool newPar = !continues || IsBullet(t) || (shortPrev && EndsWithAny(p.text, L".!?:;…")) ||
                                group[i].y - (p.y + p.h) > 0.9f * b.lineH;
            if (newPar) {
                b.text += L'\n';
                b.text += t;
            } else {
                Glue(b.text, t);
            }
        }
        out.push_back(std::move(b));
    }
    return out;
}

float UpscaleFactor(int w, int h) {
    int side = std::max(w, h);
    if (side <= 0) return 1;
    float k = 2;
    if (side * k > 4000) k = 4000.0f / side;
    return k < 1.3f ? 1.0f : k;  // прибавка в пару процентов только тратит время
}

void GrayContrast(uint32_t* pixels, int w, int h, int stride) {
    if (w <= 0 || h <= 0) return;
    int hist[256] = {};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = pixels[y * stride + x];
            int v = (((p >> 16) & 0xFF) * 77 + ((p >> 8) & 0xFF) * 150 + (p & 0xFF) * 29) >> 8;
            pixels[y * stride + x] = (uint32_t)v;
            ++hist[v];
        }
    const long long cut = (long long)w * h * 2 / 100;
    int lo = 0, hi = 255;
    for (long long seen = 0; lo < 255 && (seen += hist[lo]) <= cut;) ++lo;
    for (long long seen = 0; hi > 0 && (seen += hist[hi]) <= cut;) --hi;
    if (hi <= lo) {  // одноцветная картинка: растягивать нечего
        lo = 0;
        hi = 255;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = (int)pixels[y * stride + x];
            v = std::clamp((v - lo) * 255 / (hi - lo), 0, 255);
            pixels[y * stride + x] = 0xFF000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v;
        }
}

uint32_t EdgeColor(const uint32_t* pixels, int imageW, int imageH, int stride, float x, float y, float w, float h) {
    const int pad = 3;
    int x0 = std::clamp((int)std::floor(x) - pad, 0, imageW), y0 = std::clamp((int)std::floor(y) - pad, 0, imageH);
    int x1 = std::clamp((int)std::ceil(x + w) + pad, 0, imageW),
        y1 = std::clamp((int)std::ceil(y + h) + pad, 0, imageH);
    if (x1 - x0 < 2 || y1 - y0 < 2) return 0x1E1E2E;
    std::vector<int> ch[3];
    auto add = [&](int px, int py) {
        uint32_t p = pixels[py * stride + px];
        ch[0].push_back((p >> 16) & 0xFF);
        ch[1].push_back((p >> 8) & 0xFF);
        ch[2].push_back(p & 0xFF);
    };
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            if (yy < y0 + 2 || yy >= y1 - 2 || xx < x0 + 2 || xx >= x1 - 2) add(xx, yy);
    uint32_t out = 0;
    for (auto& c : ch) {
        std::nth_element(c.begin(), c.begin() + (std::ptrdiff_t)(c.size() / 2), c.end());
        out = out << 8 | (uint32_t)c[c.size() / 2];
    }
    return out;
}

}  // namespace ocrtext
