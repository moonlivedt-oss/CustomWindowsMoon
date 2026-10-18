#include "desk/snip.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>

#include "core/mem.h"
#include "core/fx.h"
#include "core/config.h"
#include "core/night.h"
#include "core/text.h"
#include "desk/ocr.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialSnip";
constexpr UINT WM_OCR_DONE = WM_APP + 1;
constexpr int kShowMs = 9000;
const CLSID kPng = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

std::wstring OwnerExe() {
    HWND w = GetClipboardOwner();
    if (!w) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = Lower(PathFindFileNameW(buf));
    CloseHandle(p);
    return r;
}

// DIB из буфера обмена -> картинка GDI+ (через BMP-файл в памяти).
std::unique_ptr<Bitmap> FromDib(const BYTE* dib, SIZE_T size) {
    if (size < sizeof(BITMAPINFOHEADER)) return nullptr;
    auto* bih = (const BITMAPINFOHEADER*)dib;
    DWORD colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? 1u << bih->biBitCount : 0);
    DWORD masks = (bih->biSize == sizeof(BITMAPINFOHEADER) && bih->biCompression == BI_BITFIELDS) ? 12 : 0;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = (DWORD)(sizeof(fh) + size);
    fh.bfOffBits = (DWORD)(sizeof(fh) + bih->biSize + masks + colors * 4);
    std::vector<BYTE> file(sizeof(fh) + size);
    memcpy(file.data(), &fh, sizeof(fh));
    memcpy(file.data() + sizeof(fh), dib, size);
    IStream* st = SHCreateMemStream(file.data(), (UINT)file.size());
    if (!st) return nullptr;
    std::unique_ptr<Bitmap> tmp(Bitmap::FromStream(st));
    std::unique_ptr<Bitmap> out;
    if (tmp && tmp->GetLastStatus() == Ok) {
        // своя копия: исходная держит поток
        out = std::make_unique<Bitmap>((INT)tmp->GetWidth(), (INT)tmp->GetHeight(), PixelFormat32bppRGB);
        Graphics g(out.get());
        g.DrawImage(tmp.get(), 0, 0, (INT)tmp->GetWidth(), (INT)tmp->GetHeight());
    }
    tmp.reset();
    st->Release();
    return out;
}

std::vector<uint8_t> ToPng(Bitmap* b) {
    std::vector<uint8_t> out;
    IStream* st = SHCreateMemStream(nullptr, 0);
    if (!st) return out;
    if (b->Save(st, &kPng, nullptr) == Ok) {
        STATSTG s{};
        st->Stat(&s, STATFLAG_NONAME);
        out.resize((size_t)s.cbSize.QuadPart);
        LARGE_INTEGER zero{};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        st->Read(out.data(), (ULONG)out.size(), &got);
        out.resize(got);
    }
    st->Release();
    return out;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(f);
    return ok && put == data.size();
}

bool CopyText(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    return h != nullptr;
}

}  // namespace

void SnipCard::Create(HINSTANCE inst) {
    CreateWnd(inst, kClass, L"Снимок");
    AddClipboardFormatListener(Wnd());
}

void SnipCard::Destroy() {
    if (Wnd()) RemoveClipboardFormatListener(Wnd());
    Toast::Destroy();
}

void SnipCard::Apply(const Config& cfg) {
    enabled_ = cfg.snipCard;
    theme_ = CurrentTheme(cfg);
    Invalidate();
}

std::wstring SnipCard::SaveFolder() const {
    for (const KNOWNFOLDERID* id : {&FOLDERID_Screenshots, &FOLDERID_Pictures}) {
        PWSTR p = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_CREATE, nullptr, &p))) dir = p;
        CoTaskMemFree(p);
        if (!dir.empty()) return dir;
    }
    return L"";
}

void SnipCard::OnClipboard() {
    if (!enabled_) return;
    std::wstring owner = OwnerExe();
    if (owner != L"screenclippinghost.exe" && owner != L"snippingtool.exe") return;
    if (!IsClipboardFormatAvailable(CF_DIB)) return;
    bool opened = false;
    for (int i = 0; i < 10 && !(opened = OpenClipboard(Wnd())); ++i) Sleep(20);  // «Ножницы» ещё держат буфер
    if (!opened) return;
    std::unique_ptr<Bitmap> b;
    if (HANDLE h = GetClipboardData(CF_DIB)) {
        if (auto* p = (const BYTE*)GlobalLock(h)) {
            b = FromDib(p, GlobalSize(h));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!b) return;
    shot_ = std::move(b);
    shotW_ = (int)shot_->GetWidth();
    shotH_ = (int)shot_->GetHeight();
    png_ = ToPng(shot_.get());
    status_.clear();
    savedPath_.clear();
    busy_ = false;
    Show();
}

void SnipCard::Demo() {
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, 640, 360);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, 640, 360, scr, 200, 120, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    shot_.reset(Bitmap::FromHBITMAP(bmp, nullptr));
    DeleteObject(bmp);
    shotW_ = (int)shot_->GetWidth();
    shotH_ = (int)shot_->GetHeight();
    png_ = ToPng(shot_.get());
    status_.clear();
    savedPath_.clear();
    busy_ = false;
    Show();
}

void SnipCard::Show() {
    Open(kShowMs, true);
    // превью по пропорциям снимка: широкий - ниже, высокий - уже; уменьшено заранее,
    // кадр рисуется быстро
    double ar = shotH_ > 0 ? shotW_ / (double)shotH_ : 1.5;
    pw_ = (float)std::clamp(100 * ar, 80.0, 136.0);
    ph_ = (float)std::clamp(pw_ / ar, 64.0, 112.0);
    int pw = (int)std::lround(pw_ * k_), ph = (int)std::lround(ph_ * k_);
    thumb_ = std::make_unique<Bitmap>(pw, ph, PixelFormat32bppPARGB);
    {
        Graphics g(thumb_.get());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(255, 14, 15, 20));
        float s = std::max(pw / (float)shotW_,
                           ph / (float)shotH_);  // заполнить рамку, лишнее срезать
        float w = shotW_ * s, h = shotH_ * s;
        g.DrawImage(shot_.get(), RectF((pw - w) / 2, (ph - h) / 2, w, h));
    }
    Invalidate();
}

RectF SnipCard::R(int id) const {
    RectF body = Body();
    float K = k(), x0 = body.X + (16 + pw_ + 16) * K;
    switch (id) {
        case H_PREVIEW:
            return RectF(body.X + 16 * K, body.Y + 16 * K, pw_ * K, ph_ * K);
        case H_SAVE:
            return RectF(x0, body.GetBottom() - (16 + 32) * K, ((kW - 16) * K - (x0 - body.X) - 8 * K) / 2, 32 * K);
        case H_TEXT:
            return RectF(R(H_SAVE).GetRight() + 8 * K, body.GetBottom() - (16 + 32) * K, R(H_SAVE).Width, 32 * K);
    }
    return RectF();
}

float SnipCard::BodyHeight(Graphics& g) {
    float K = k();
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12 * K, FontStyleRegular, UnitPixel);
    float tw = (kW - 16 - pw_ - 16 - 16) * K;
    float st = std::min(TextHeight(g, status_, fS, tw), 2 * 17 * K) / K;
    float text = 16 + 21 + 2 + 17 + (st > 0 ? 6 + st : 0) + 12 + 32 + 16;
    return std::max(16 + ph_ + 16 + 2, text);
}

void SnipCard::Paint(Graphics& g, const RectF& body) {
    float K = k();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(theme_.ac);
    // превью
    RectF pv = R(H_PREVIEW);
    if (Pressed(H_PREVIEW)) pv = Inset(pv, 1.5f * K, 1.5f * K);
    GraphicsPath pp;
    RoundRectPath(pp, pv, 10 * K);
    if (thumb_) {
        GraphicsState st = g.Save();
        g.SetClip(&pp);
        g.SetInterpolationMode(InterpolationModeBilinear);
        g.DrawImage(thumb_.get(), pv);
        float hv = HoverOf(H_PREVIEW);
        if (hv > 0) {  // наведение: затемнение и подпись «Открыть»
            SolidBrush hb(Color((BYTE)(110 * hv), 0, 0, 0));
            g.FillRectangle(&hb, pv);
            Font fo(Family(), 12 * K, FontStyleRegular, UnitPixel);
            SolidBrush ob(Color((BYTE)(255 * hv), 255, 255, 255));
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"Открыть", -1, &fo, pv, &cf, &ob);
        }
        g.Restore(st);
    }
    Pen pr(Color(56, 255, 255, 255), 1 * K);
    g.DrawPath(&pr, &pp);

    float x0 = R(H_SAVE).X, tw = body.GetRight() - 16 * K - x0;
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12 * K, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.2));
    g.DrawString(L"Снимок области", -1, &fT, RectF(x0, body.Y + 15 * K, tw - 26 * K, 22 * K), &sf, &tB);
    std::wstring size = std::to_wstring(shotW_) + L" x " + std::to_wstring(shotH_) + L" px";
    if (!savedPath_.empty()) size += L"   |   сохранён";
    g.DrawString(size.c_str(), -1, &fS, RectF(x0, body.Y + 38 * K, tw, 18 * K), &sf, &mB);
    if (!status_.empty()) {
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        SolidBrush sB(busy_         ? muted
                      : statusGood_ ? MixColor(ac, Color(255, 255, 255), 0.4)
                                    : Color(255, 240, 140, 120));
        g.DrawString(status_.c_str(), -1, &fS, RectF(x0, body.Y + 61 * K, tw, 2 * 17 * K), &wf, &sB);
    }
    DrawButton(g, R(H_SAVE), savedPath_.empty() ? L"Сохранить" : L"Папка", false, H_SAVE);
    std::wstring text =
        busy_ ? L"Читаю" + std::wstring((size_t)((GetTickCount64() - busySince_) / 350 % 4), L'.') : L"Текст";
    DrawButton(g, R(H_TEXT), text, true, H_TEXT);
}

int SnipCard::HitContent(const PointF& p) const {
    for (int id : {H_SAVE, H_TEXT, H_PREVIEW})
        if (R(id).Contains(p)) return id;
    return H_NONE;
}

void SnipCard::Save(bool thenOpen) {
    if (png_.empty()) return;
    if (!savedPath_.empty()) {
        if (thenOpen) {
            std::wstring arg = L"/select,\"" + savedPath_ + L"\"";
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[80];
    swprintf(name, 80, L"Снимок %04d-%02d-%02d %02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    std::wstring dir = SaveFolder();
    std::wstring path = dir + L"\\" + name;
    if (!dir.empty() && WriteFileBytes(path, png_)) {
        savedPath_ = path;
        status_ = std::wstring(L"Сохранено: ") + name;
        statusGood_ = true;
    } else {
        status_ = L"Не удалось сохранить";
        statusGood_ = false;
    }
    Invalidate();
}

void SnipCard::Click(int id) {
    if (id == H_SAVE) Save(true);
    if (id == H_TEXT && !busy_ && !png_.empty()) {
        busy_ = true;
        busySince_ = GetTickCount64();
        status_ = L"Распознаю текст...";
        statusGood_ = true;
        ocr::RecognizeAsync(png_, Wnd(), WM_OCR_DONE);
        Invalidate();
    }
    if (id == H_PREVIEW && !png_.empty()) {
        // открыть снимок в просмотрщике: сохранённый файл или временный
        std::wstring p = savedPath_;
        if (p.empty()) {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            p = std::wstring(tmp) + L"radial-snip.png";
            if (!WriteFileBytes(p, png_)) return;
        }
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool SnipCard::Message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        OnClipboard();
        return true;
    }
    if (msg != WM_OCR_DONE) return false;
    std::unique_ptr<std::wstring> text((std::wstring*)lp);
    busy_ = false;
    if (wp == 0 && text && !text->empty()) {
        CopyText(Wnd(), *text);
        int lines = 1 + (int)std::count(text->begin(), text->end(), L'\n');
        std::wstring first = text->substr(0, text->find_first_of(L"\r\n"));
        status_ = L"Текст в буфере, " + Plural(lines, L"строка", L"строки", L"строк") + L": " + first;
        statusGood_ = true;
    } else {
        status_ = wp == 0 ? std::wstring(L"Текста на снимке не нашлось")
                          : (text ? *text : std::wstring(L"Распознать не получилось"));
        statusGood_ = false;
    }
    if (Visible()) Open(kShowMs, true);  // результат виден ещё несколько секунд
    Invalidate();
    return true;
}
