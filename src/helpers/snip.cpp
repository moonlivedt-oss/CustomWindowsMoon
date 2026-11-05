#include "core/usage.h"
#include "helpers/snip.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/mem.h"
#include "core/fx.h"
#include "core/config.h"
#include "core/night.h"
#include "core/text.h"
#include "helpers/resume.h"
#include "helpers/markup.h"
#include "helpers/ocr.h"
#include "helpers/markup_logic.h"
#include "helpers/pin.h"
#include "helpers/snipcompare.h"
#include "helpers/snipname.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialSnip";
constexpr UINT WM_OCR_DONE = WM_APP + 1;
constexpr int kShowMs = 9000;
constexpr wchar_t kFoldersKey[] = L"Software\\MoonRadial\\SnipFolders";
constexpr ULONGLONG kForgetMs = 30 * 60000ull;  // сколько лента помнит снимки, пока карточки нет на экране
constexpr float kFrameW = 136, kFrameMaxH = 112;  // рамка превью, dip: высокий снимок поднимает её до kFrameMaxH
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
    // заголовок и палитра приходят от чужой программы: размеры проверяем, а не верим им
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > size || bih->biBitCount > 32) return nullptr;
    DWORD colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? 1u << bih->biBitCount : 0);
    if (colors > 256) return nullptr;
    // маски цветов идут после короткого заголовка; у V4/V5 они внутри заголовка
    DWORD masks = 0;
    if (bih->biSize == sizeof(BITMAPINFOHEADER))
        masks = bih->biCompression == BI_BITFIELDS ? 12 : bih->biCompression == 6 /* BI_ALPHABITFIELDS */ ? 16 : 0;
    ULONGLONG off = sizeof(BITMAPFILEHEADER) + (ULONGLONG)bih->biSize + masks + colors * 4ull;
    if (off >= sizeof(BITMAPFILEHEADER) + size) return nullptr;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = (DWORD)(sizeof(fh) + size);
    fh.bfOffBits = (DWORD)off;
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

std::wstring g_testOutput;

bool CopyText(HWND owner, const std::wstring& s) {
    if (!g_testOutput.empty()) {
        std::string utf8(WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), utf8.data(), (int)utf8.size(), nullptr, nullptr);
        return WriteFileBytes(g_testOutput + L".txt", std::vector<uint8_t>(utf8.begin(), utf8.end()));
    }
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

namespace snipimg {
std::unique_ptr<Bitmap> FromDib(const BYTE* dib, SIZE_T size) { return ::FromDib(dib, size); }
std::vector<uint8_t> ToPng(Bitmap* b) { return ::ToPng(b); }
bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& data) { return WriteFileBytes(path, data); }

std::unique_ptr<Bitmap> FromPng(const std::vector<uint8_t>& png) {
    if (png.empty()) return nullptr;
    IStream* st = SHCreateMemStream(png.data(), (UINT)png.size());
    if (!st) return nullptr;
    std::unique_ptr<Bitmap> out;
    {
        std::unique_ptr<Bitmap> tmp(Bitmap::FromStream(st));
        if (tmp && tmp->GetLastStatus() == Ok) {
            // своя копия: исходная держит поток
            out = std::make_unique<Bitmap>((INT)tmp->GetWidth(), (INT)tmp->GetHeight(), PixelFormat32bppRGB);
            Graphics g(out.get());
            g.DrawImage(tmp.get(), 0, 0, (INT)tmp->GetWidth(), (INT)tmp->GetHeight());
        }
    }
    st->Release();
    return out;
}

bool TextToClipboard(HWND owner, const std::wstring& text) { return CopyText(owner, text); }
void SetTestOutput(const std::wstring& file) { g_testOutput = file; }
bool TestMode() { return !g_testOutput.empty(); }

bool ToClipboard(HWND owner, Bitmap* b) {
    if (!g_testOutput.empty()) return WriteFileBytes(g_testOutput, ::ToPng(b));
    int w = (int)b->GetWidth(), h = (int)b->GetHeight();
    Rect all(0, 0, w, h);
    BitmapData d{};
    if (b->LockBits(&all, ImageLockModeRead, PixelFormat32bppRGB, &d) != Ok) return false;
    const SIZE_T bytes = sizeof(BITMAPINFOHEADER) + (SIZE_T)w * h * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        auto* head = (BITMAPINFOHEADER*)GlobalLock(mem);
        *head = BITMAPINFOHEADER{};
        head->biSize = sizeof(BITMAPINFOHEADER);
        head->biWidth = w;
        head->biHeight = h;  // снизу вверх, как принято у DIB
        head->biPlanes = 1;
        head->biBitCount = 32;
        head->biCompression = BI_RGB;
        auto* out = (uint32_t*)(head + 1);
        for (int y = 0; y < h; ++y) {
            auto* row = (const uint32_t*)((const BYTE*)d.Scan0 + (ptrdiff_t)y * d.Stride);
            uint32_t* to = out + (ptrdiff_t)(h - 1 - y) * w;
            for (int x = 0; x < w; ++x) to[x] = row[x] | 0xFF000000u;  // непрозрачно: иначе часть программ покажет пустоту
        }
        GlobalUnlock(mem);
    }
    b->UnlockBits(&d);
    if (!mem) return false;
    bool opened = false;
    for (int i = 0; i < 10 && !(opened = OpenClipboard(owner)); ++i) Sleep(20);
    if (!opened) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
    if (!ok) GlobalFree(mem);
    CloseClipboard();
    return ok;
}

static std::wstring TempFolder() {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return L"";
    return std::wstring(tmp) + L"RadialSnip";
}

std::wstring TempFile(const std::wstring& name) {
    std::wstring dir = TempFolder();
    if (dir.empty()) return L"";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}

void PurgeTemp(ULONGLONG olderThanMs) {
    std::wstring dir = TempFolder();
    if (dir.empty()) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now{{nowFt.dwLowDateTime, nowFt.dwHighDateTime}};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER at{{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        if (now.QuadPart < at.QuadPart || (now.QuadPart - at.QuadPart) / 10000 >= olderThanMs)
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace snipimg

void SnipCard::Create(HINSTANCE inst) {
    CreateWnd(inst, kClass, L"Снимок");
    AddClipboardFormatListener(Wnd());
    snipimg::PurgeTemp(3600000);  // прошлый запуск мог оставить файлы для перетаскивания
}

void SnipCard::Destroy() {
    if (Wnd()) RemoveClipboardFormatListener(Wnd());
    markup::Close();
    snipcompare::Close();
    pin::CloseAll();
    snipimg::PurgeTemp(0);
    Toast::Destroy();
}

void SnipCard::Apply(const Config& cfg) {
    enabled_ = cfg.snipCard;
    theme_ = CurrentTheme(cfg);
    uiColors_ = UiPalette(cfg);
    Invalidate();
}

namespace {

std::wstring ExeOf(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = PathFindFileNameW(buf);
    CloseHandle(p);
    return r;
}

// Окно, из которого сняли: верхнее видимое окно с заголовком, не «Ножницы», не стол и не своё (EnumWindows
// идёт сверху вниз по Z-порядку)
HWND SourceWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked || GetWindowTextLengthW(w) == 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            std::wstring exe = Lower(ExeOf(w));
            if (exe == L"screenclippinghost.exe" || exe == L"snippingtool.exe" || exe == L"shellexperiencehost.exe" ||
                exe == L"searchhost.exe" || exe == L"startmenuexperiencehost.exe")
                return TRUE;
            wchar_t cls[64];
            GetClassNameW(w, cls, 64);
            if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return TRUE;
            *(HWND*)lp = w;
            return FALSE;
        },
        (LPARAM)&found);
    return found;
}

bool HasAny(const std::wstring& dir, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        std::wstring p = dir + L"\\" + n;
        if (wcschr(n, L'*')) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(p.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                return true;
            }
        } else if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Л29: окно, из которого сняли, -> понятное имя («MyGame Player.cpp») и папка: проект из VS Code, Unreal или
// папка Проводника с тем же именем в заголовке (resume::Now знает, какие открыты); у проекта - его screenshots
void SnipCard::FindTarget() {
    context_.clear();
    targetDir_.clear();
    targetLabel_.clear();
    HWND w = SourceWindow();
    if (!w) return;
    wchar_t title[512] = {};
    GetWindowTextW(w, title, 512);
    std::wstring t = title, lt = Lower(t);
    context_ = snipname::Context(t, ExeOf(w));
    const resume::Thing* best = nullptr;
    std::vector<resume::Thing> open = resume::Now();
    for (const resume::Thing& th : open) {
        if (th.kind == resume::Thing::APP || th.label.size() < 2) continue;
        if (lt.find(Lower(th.label)) == std::wstring::npos) continue;
        if (!best || th.label.size() > best->label.size()) best = &th;
    }
    if (!best) {
        targetDir_ = RememberedFolder(L"");
        return;
    }
    std::wstring folder = best->path;
    if (best->kind == resume::Thing::UNREAL) folder = folder.substr(0, folder.find_last_of(L'\\'));
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    bool project = best->kind != resume::Thing::FOLDER ||
                   HasAny(folder, {L".git", L"package.json", L"CMakeLists.txt", L"*.uproject", L"*.sln", L"Cargo.toml",
                                   L"pyproject.toml"});
    targetLabel_ = best->label;
    std::wstring chosen = RememberedFolder(targetLabel_);
    targetDir_ = chosen.empty() ? snipname::TargetDir(folder, project) : chosen;
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
    TakeShot(std::move(b));
    FindTarget();
    Show();
}

void SnipCard::Stash() {
    if (current_ < 0 || current_ >= (int)recent_.size()) return;
    Recent& r = recent_[(size_t)current_];
    r.png = std::move(png_);
    r.w = shotW_;
    r.h = shotH_;
    r.savedPath = savedPath_;
    r.context = context_;
    r.targetDir = targetDir_;
    r.targetLabel = targetLabel_;
    png_.clear();
}

void SnipCard::TakeShot(std::unique_ptr<Bitmap> shot) {
    Stash();
    if ((int)recent_.size() >= kRecent) {  // звёзды остаются: уходит самый старый без звезды
        std::vector<bool> keep;
        for (const Recent& r : recent_) keep.push_back(r.keep);
        recent_.erase(recent_.begin() + markup::EvictIndex(keep));
    }
    recent_.emplace_back();
    recent_.back().serial = nextSerial_++;
    current_ = (int)recent_.size() - 1;
    lastShotAt_ = GetTickCount64();
    context_.clear();
    targetDir_.clear();
    targetLabel_.clear();
    shot_ = std::move(shot);
    shotW_ = (int)shot_->GetWidth();
    shotH_ = (int)shot_->GetHeight();
    png_ = ToPng(shot_.get());
    status_.clear();
    savedPath_.clear();
    if (busy_) ++ocrStale_;  // его текст ещё придёт, но к новому снимку не относится
    busy_ = false;
    saveFill_ = 1;
    textFill_ = 0;
    jumpT_ = -1;
    flash_ = 0;
    laterAt_ = 0;  // новый снимок заменяет отложенный
}

void SnipCard::MenuChoose(int minutes, bool) {
    if (minutes > 0) laterAt_ = GetTickCount64() + minutes * 60000ull;
}

// Время вышло: снимки без звезды забываются. Отмеченные живут, пока программа запущена
void SnipCard::Forget() {
    bool any = false;
    for (const Recent& r : recent_) any = any || r.keep;
    if (!any) {
        recent_.clear();
        current_ = -1;
        png_.clear();
        png_.shrink_to_fit();
        thumb_.reset();
        shot_.reset();
        savedPath_.clear();
        status_.clear();
        return;
    }
    Stash();
    current_ = -1;
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [](const Recent& r) { return !r.keep; }), recent_.end());
    thumb_.reset();
    // показать снова - первый из оставшихся
    Select(0);
    lastShotAt_ = GetTickCount64();
}

std::vector<uint8_t> SnipCard::PngOf(int index) const {
    if (index == current_) return png_;
    if (index >= 0 && index < (int)recent_.size()) return recent_[(size_t)index].png;
    return {};
}

void SnipCard::ToggleKeep() {
    if (current_ < 0 || current_ >= (int)recent_.size()) return;
    bool& keep = recent_[(size_t)current_].keep;
    keep = !keep;
    status_ = keep ? L"Снимок останется в памяти, пока запущена программа" : L"Снимок забудется через полчаса";
    statusGood_ = true;
    Hold(kShowMs);
    Invalidate();
}

void SnipCard::CompareWith(int index) {
    if (index == current_ || index < 0 || index >= (int)recent_.size()) return;
    // слева - более ранний снимок, справа - более поздний: «было» и «стало»
    int first = std::min(index, current_), second = std::max(index, current_);
    snipcompare::Show(uiColors_, snipimg::FromPng(PngOf(first)), snipimg::FromPng(PngOf(second)));
}

std::wstring SnipCard::RememberedFolder(const std::wstring& label) const {
    const std::wstring name = label.empty() ? L"*" : label;
    if (snipimg::TestMode()) {
        auto it = testFolders_.find(name);
        return it == testFolders_.end() ? L"" : it->second;
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kFoldersKey, name.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES ? L"" : buf;  // папку могли удалить
}

// Куда класть снимки этого проекта (или вообще, если снимали не из проекта). Запоминается на следующие разы
void SnipCard::ChooseFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(targetLabel_.empty() ? L"Куда сохранять снимки" : (L"Куда сохранять снимки: " + targetLabel_).c_str());
    Hold(0);
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring folder = path;
            const std::wstring name = targetLabel_.empty() ? L"*" : targetLabel_;
            if (snipimg::TestMode())
                testFolders_[name] = folder;
            else
                RegSetKeyValueW(HKEY_CURRENT_USER, kFoldersKey, name.c_str(), REG_SZ, folder.c_str(),
                                (DWORD)((folder.size() + 1) * sizeof(wchar_t)));
            targetDir_ = folder;
            status_ = L"Снимки теперь в: " + folder;
            statusGood_ = true;
        }
        CoTaskMemFree(path);
        if (item) item->Release();
    }
    dialog->Release();
    Hold(kShowMs);
    Invalidate();
}

void SnipCard::Idle(ULONGLONG now) {
    if (!laterAt_ && !Visible() && !editing_ && !recent_.empty() && now - lastShotAt_ > kForgetMs) Forget();
    if (!laterAt_ || now < laterAt_ || Visible() || fx::Game()) return;
    laterAt_ = 0;
    Open(kShowMs, true);
}

bool SnipCard::Animate(double dtMs) {
    double dt = dtMs / fx::Dur(1);  // доля обычного времени; в игре - сразу конец
    bool saved = !savedPath_.empty(), moved = busy_;
    auto toward = [&](float& v, float target, double ms) {
        if (v == target) return;
        float step = (float)(dt / ms);
        v = v < target ? std::min(target, v + step) : std::max(target, v - step);
        moved = true;
    };
    toward(saveFill_, saved ? 0.0f : 1.0f, 420);
    // заливка уходит из первой кнопки и только потом приходит во вторую
    if (!saved || saveFill_ < 0.55f) toward(textFill_, saved ? 1.0f : 0.0f, 420);
    if (jumpT_ >= 0) {
        jumpT_ += (float)(dt / 340);
        moved = true;
        if (jumpT_ >= 1) {  // капля долетела: сохранить, кнопка отзывается вместе с карточкой
            jumpT_ = -1;
            Kick(-0.7f);
            Save(false);
        }
    }
    if (status_ != lastStatus_) {
        lastStatus_ = status_;
        statusA_ = status_.empty() ? 1.0f : 0.0f;
        if (!status_.empty()) Kick(0.9f);
    }
    toward(statusA_, 1, 260);
    toward(flash_, 0, 700);
    return moved;
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
    std::unique_ptr<Bitmap> b(Bitmap::FromHBITMAP(bmp, nullptr));
    DeleteObject(bmp);
    TakeShot(std::move(b));
    Show();
}

void SnipCard::DemoShot(int w, int h, bool saved) {
    auto b = std::make_unique<Bitmap>(w, h, PixelFormat32bppRGB);
    {
        Graphics g(b.get());
        LinearGradientBrush fill(PointF(0, 0), PointF((REAL)w, (REAL)h), Color(255, 58, 96, 150),
                                 Color(255, 214, 120, 96));
        g.FillRectangle(&fill, 0, 0, w, h);
        // рамка до самого края: на снимке карточки сразу видно, срезано ли превью
        Pen edge(Color(255, 255, 255, 255), std::max(4.0f, std::min(w, h) / 40.0f));
        g.DrawRectangle(&edge, 0, 0, w - 1, h - 1);
        // токен: проверкам разметки нужно, чтобы распознавание нашло что скрыть
        if (w >= 800) {
            FontFamily fam(L"Consolas");
            Font font(&fam, h * 0.035f, FontStyleRegular, UnitPixel);
            SolidBrush ink(Color(255, 240, 240, 245));
            g.DrawString(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK", -1, &font, PointF(w * 0.1f, h * 0.11f), &ink);
            g.DrawString(L"Обычный текст для распознавания", -1, &font, PointF(w * 0.1f, h * 0.33f), &ink);
        }
    }
    TakeShot(std::move(b));
    targetLabel_ = L"RadialMenu";
    if (saved) {
        savedPath_ = L"RadialMenu widget.cpp 2026-11-04 12-30.png";
        status_ = L"Сохранено в RadialMenu: " + savedPath_;
        statusGood_ = true;
    }
    Show();
}

void SnipCard::DemoSave(const std::wstring& dir) {
    targetDir_ = dir;
    Click(H_SAVE);
}

void SnipCard::DemoBusy() {
    busy_ = true;
    busySince_ = GetTickCount64();
    Hold(0);
    Invalidate();
}

void SnipCard::Show() {
    usage::Hit(L"snip_card");
    Open(kShowMs, true);
    MakeThumbs();
}

void SnipCard::MakeThumbs() {
    if (!shot_) shot_ = snipimg::FromPng(png_);
    if (!shot_) return;
    // картинка в рамке целиком, уменьшена заранее: кадр рисуется быстро
    snipname::FitPreview(shotW_, shotH_, kFrameW, kFrameMaxH, pw_, ph_);
    int pw = std::max(1, (int)std::lround(pw_ * k_)), ph = std::max(1, (int)std::lround(ph_ * k_));
    thumb_ = std::make_unique<Bitmap>(pw, ph, PixelFormat32bppPARGB);
    {
        Graphics g(thumb_.get());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(shot_.get(), RectF(0, 0, (REAL)pw, (REAL)ph));
    }
    if (current_ >= 0 && current_ < (int)recent_.size()) {
        // миниатюра ленты заполняет ячейку без полей: важно узнать снимок, а не увидеть его целиком
        int sw = std::max(1, (int)std::lround(44 * k_)), sh = std::max(1, (int)std::lround(32 * k_));
        auto small = std::make_unique<Bitmap>(sw, sh, PixelFormat32bppPARGB);
        {
            Graphics g(small.get());
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            float s = std::max((float)sw / shotW_, (float)sh / shotH_), cw = sw / s, ch = sh / s;
            g.DrawImage(shot_.get(), RectF(0, 0, (REAL)sw, (REAL)sh), (shotW_ - cw) / 2, (shotH_ - ch) / 2, cw, ch, UnitPixel);
        }
        recent_[(size_t)current_].thumb = std::move(small);
    }
    shot_.reset();  // снимок целиком уже в png_: распакованный занимал бы десятки мегабайт
    Invalidate();
}

void SnipCard::Select(int index) {
    if (index == current_ || index < 0 || index >= (int)recent_.size()) return;
    if (busy_) ++ocrStale_;  // текст придёт от другого снимка: в буфер и в карточку он не пойдёт
    busy_ = false;
    Stash();
    current_ = index;
    Recent& r = recent_[(size_t)index];
    png_ = std::move(r.png);
    r.png.clear();
    shotW_ = r.w;
    shotH_ = r.h;
    savedPath_ = r.savedPath;
    context_ = r.context;
    targetDir_ = r.targetDir;
    targetLabel_ = r.targetLabel;
    status_ = savedPath_.empty() ? L"" : L"Сохранено: " + savedPath_.substr(savedPath_.find_last_of(L'\\') + 1);
    statusGood_ = true;
    saveFill_ = savedPath_.empty() ? 1.0f : 0.0f;
    textFill_ = savedPath_.empty() ? 0.0f : 1.0f;
    jumpT_ = -1;
    flash_ = 0;
    MakeThumbs();
    Hold(kShowMs);
}

void SnipCard::OpenEditor() {
    if (png_.empty() || current_ < 0) return;
    uint64_t serial = recent_[(size_t)current_].serial;
    markup::Callbacks cb;
    cb.done = [this, serial](std::unique_ptr<Bitmap> image, markup::Finish how) {
        ApplyEdit(serial, std::move(image), how == markup::Finish::ApplyAndSave);
    };
    cb.closed = [this] {
        editing_ = false;
        if (!png_.empty() && !Visible()) Open(kShowMs, true);  // вернулась: снимок и его кнопки снова под рукой
    };
    editing_ = true;
    markup::Open(uiColors_, png_, std::move(cb));
    Hide();  // карточка поверх всех окон и закрывала бы кнопки редактора; вернётся, когда он закроется
}

void SnipCard::ApplyEdit(uint64_t serial, std::unique_ptr<Bitmap> image, bool save) {
    int index = -1;
    for (size_t i = 0; i < recent_.size(); ++i)
        if (recent_[i].serial == serial) index = (int)i;
    if (index < 0) {  // за время разметки лента ушла вперёд: снимок с правками становится новым
        TakeShot(std::move(image));
        index = current_;
        image = std::move(shot_);
    } else if (index != current_) {
        Select(index);
    }
    shot_ = std::move(image);
    shotW_ = (int)shot_->GetWidth();
    shotH_ = (int)shot_->GetHeight();
    png_ = ToPng(shot_.get());
    // прежний файл - без правок: сохранение ляжет новым файлом, а не поверх
    savedPath_.clear();
    saveFill_ = 1;
    textFill_ = 0;
    status_ = save ? L"" : L"Правки на снимке и в буфере";
    statusGood_ = true;
    if (!Visible()) Open(kShowMs, true);
    MakeThumbs();
    if (save) Save(false);
    Invalidate();
}

void SnipCard::PinCurrent() {
    if (auto image = snipimg::FromPng(png_)) pin::Show(*image);
}

namespace {
class DragSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // на стеке, живёт дольше DoDragDrop
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
}  // namespace

// Превью тянут в браузер, чат или редактор: им нужен файл. Сохранённый снимок - его файл, иначе временный с тем же
// понятным именем, что и при «Сохранить»
void SnipCard::StartDrag() {
    if (png_.empty()) return;
    std::wstring file = savedPath_;
    if (file.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        file = snipimg::TempFile(snipname::FileName(context_, t));
        if (file.empty() || !WriteFileBytes(file, png_)) return;
    }
    IShellItem* item = nullptr;
    IDataObject* data = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
        Hold(0);  // карточка не уходит из-под курсора посреди переноса
        DragSource source;
        DWORD effect = 0;
        DoDragDrop(data, &source, DROPEFFECT_COPY, &effect);
        Hold(kShowMs);
    }
    if (data) data->Release();
    if (item) item->Release();
}

RectF SnipCard::R(int id) const {
    RectF body = Body();
    float K = k(), x0 = body.X + (16 + kFrameW + 16) * K;
    switch (id) {
        case H_PREVIEW:
            return RectF(body.X + 16 * K, body.Y + 16 * K, kFrameW * K, body.Height - (32 + StripH()) * K);
        case H_SAVE:
            return RectF(x0, body.GetBottom() - (16 + 32 + StripH()) * K,
                         ((kW - 16) * K - (x0 - body.X) - (8 + 8 + 32) * K) / 2, 32 * K);
        case H_TEXT:
            return RectF(R(H_SAVE).GetRight() + 8 * K, R(H_SAVE).Y, R(H_SAVE).Width, 32 * K);
        case H_PIN:
            return RectF(R(H_TEXT).GetRight() + 8 * K, R(H_SAVE).Y, 32 * K, 32 * K);
        case H_STAR:  // в углу превью: звезда нужна и единственному снимку, когда ленты ещё нет
            return RectF(body.X + (16 + 5) * K, body.Y + (16 + 5) * K, 22 * K, 22 * K);
        case H_COMPARE:
            return RectF(body.GetRight() - (16 + 96) * K, body.GetBottom() - (16 + 32) * K, 96 * K, 32 * K);
    }
    if (id >= H_STRIP && id < H_STRIP + kRecent)
        return RectF(body.X + (16 + (id - H_STRIP) * (44 + 6)) * K, body.GetBottom() - (16 + 32) * K, 44 * K, 32 * K);
    return RectF();
}

float SnipCard::BodyHeight(Graphics& g) {
    float K = k();
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12 * K, FontStyleRegular, UnitPixel);
    float tw = (kW - 16 - kFrameW - 16 - 16) * K;
    float st = std::min(TextHeight(g, status_, fS, tw), 2 * 17 * K) / K;
    float text = 16 + 21 + 2 + 17 + (st > 0 ? 6 + st : 0) + 12 + 32 + 16;
    return std::max(16 + ph_ + 16, text) + StripH();
}

void SnipCard::Paint(Graphics& g, const RectF& body) {
    float K = k();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(theme_.ac);
    // превью выпрыгивает с перелётом
    RectF pv = R(H_PREVIEW);
    if (Pressed(H_PREVIEW)) pv = Inset(pv, 1.5f * K, 1.5f * K);
    float pop = 0.55f + 0.45f * (float)EaseOutBack(Reveal(0.05f, 0.55f), 1.7);
    GraphicsState popped = g.Save();
    g.TranslateTransform(pv.X + pv.Width / 2, pv.Y + pv.Height / 2);
    g.ScaleTransform(pop, pop);
    g.TranslateTransform(-(pv.X + pv.Width / 2), -(pv.Y + pv.Height / 2));
    DrawFaded(g, Grow(pv, 2 * K), Reveal(0.05f, 0.15f), [&](Graphics& gp) {
        GraphicsPath pp;
        RoundRectPath(pp, pv, 12 * K);
        if (thumb_) {
            GraphicsState st = gp.Save();
            gp.SetClip(&pp, CombineModeIntersect);
            // рамка темнее карточки: снимок другой формы лежит в ней с полями, а не обрезается
            SolidBrush matte(Color(255, 14, 15, 20));
            gp.FillRectangle(&matte, pv);
            float iw = pw_ * K, ih = ph_ * K;
            gp.SetInterpolationMode(InterpolationModeBilinear);
            gp.DrawImage(thumb_.get(), RectF(pv.X + (pv.Width - iw) / 2, pv.Y + (pv.Height - ih) / 2, iw, ih));
            if (busy_) {  // распознавание: сверху вниз идёт волна
                float t = (float)((GetTickCount64() % 1100) / 1100.0), band = 42 * K;
                float y = pv.Y - band + t * (pv.Height + band * 1.5f);
                PointF pts[26];
                pts[0] = PointF(pv.X, y);
                pts[1] = PointF(pv.GetRight(), y);
                for (int i = 0; i < 24; ++i) {
                    float x = pv.GetRight() - pv.Width * i / 23;
                    pts[2 + i] = PointF(x, y + band + 3 * K * (float)std::sin(x / (11 * K) + t * 9));
                }
                LinearGradientBrush wave(PointF(0, y - 1), PointF(0, y + band + 4 * K), WithAlpha(ac, 0),
                                         WithAlpha(ac, 170));
                gp.FillPolygon(&wave, pts, 26);
                Pen crest(MixColor(ac, Color(255, 255, 255), 0.55), 2 * K);
                crest.SetLineJoin(LineJoinRound);
                gp.DrawLines(&crest, pts + 2, 24);
            }
            float hv = HoverOf(H_PREVIEW);
            if (hv > 0) {  // наведение: затемнение и подпись
                SolidBrush hb(Color((BYTE)(110 * hv), 0, 0, 0));
                gp.FillRectangle(&hb, pv);
                Font fo(Family(), 12 * K, FontStyleRegular, UnitPixel);
                SolidBrush ob(Color((BYTE)(255 * hv), 255, 255, 255));
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                gp.DrawString(L"Разметка", -1, &fo, pv, &cf, &ob);
            }
            gp.Restore(st);
        }
        Pen pr(Color(56, 255, 255, 255), 1 * K);
        gp.DrawPath(&pr, &pp);
        if (flash_ > 0) {  // текст готов - рамка вспыхивает
            Pen fp(WithAlpha(ac, (BYTE)(255 * flash_)), 2.2f * K);
            gp.DrawPath(&fp, &pp);
        }
    });
    g.Restore(popped);

    float x0 = R(H_SAVE).X, tw = body.GetRight() - 16 * K - x0;
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12 * K, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.2));
    // строки въезжают справа одна за другой
    auto rise = [&](float delay, const RectF& area, const std::function<void(Graphics&)>& paint) {
        GraphicsState st = g.Save();
        g.TranslateTransform(16 * K * (1 - (float)EaseOutBack(Reveal(delay, 0.5f), 1.3)), 0);
        DrawFaded(g, area, Reveal(delay, 0.2f), paint);
        g.Restore(st);
    };
    RectF titleR(x0, body.Y + 15 * K, tw - 56 * K, 22 * K);
    rise(0.1f, titleR, [&](Graphics& gt) { gt.DrawString(L"Снимок области", -1, &fT, titleR, &sf, &tB); });
    std::wstring size = std::to_wstring(shotW_) + L" × " + std::to_wstring(shotH_) + L" px";
    if (savedPath_.empty() && !targetLabel_.empty())
        size += L"  ·  в " + targetLabel_;  // Л29: куда ляжет по «Сохранить»; после сохранения об этом скажет итог
    RectF sizeR(x0, body.Y + 38 * K, tw, 18 * K);
    // у значков нет подписи, у правого щелчка - кнопки: объясняем здесь
    if (HoverOf(H_PIN) > 0.5f) size = L"Закрепить поверх окон";
    if (HoverOf(H_SAVE) > 0.5f && savedPath_.empty()) size = L"Правый щелчок: выбрать папку";
    if (HoverOf(H_STAR) > 0.5f) size = L"Не забывать через полчаса";
    if (HoverOf(H_COMPARE) > 0.5f) size = L"Сравнить с соседним снимком";
    for (int i = 0; i < (int)recent_.size(); ++i)
        if (i != current_ && HoverOf(H_STRIP + i) > 0.5f) size = L"Щелчок: показать. Правый: сравнить";
    rise(0.15f, sizeR, [&](Graphics& gt) { gt.DrawString(size.c_str(), -1, &fS, sizeR, &sf, &mB); });
    if (!status_.empty()) {
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        Color sc = busy_ ? muted : statusGood_ ? MixColor(ac, Color(255, 255, 255), 0.4) : Color(255, 240, 140, 120);
        SolidBrush sB(WithAlpha(sc, (BYTE)(255 * statusA_)));
        g.DrawString(status_.c_str(), -1, &fS, RectF(x0, body.Y + 61 * K, tw, 2 * 17 * K), &wf, &sB);
    }
    // заливка перетекает: из «Сохранить» уходит вправо, в «Текст» приходит слева
    auto fillButton = [&](Graphics& gb, const RectF& r, const std::wstring& label, int id, float fill, bool fromLeft) {
        float f = fill * fill * (3 - 2 * fill);
        if (f < 1) DrawButton(gb, r, label, false, id);
        if (f > 0) {
            GraphicsState st = gb.Save();
            float w = (r.Width + 2) * f;
            gb.IntersectClip(RectF(fromLeft ? r.X - 1 : r.GetRight() + 1 - w, r.Y - 1, w, r.Height + 2));
            DrawButton(gb, r, label, true, id);
            gb.Restore(st);
        }
    };
    RectF rs = R(H_SAVE), rt = R(H_TEXT);
    RectF rp = R(H_PIN);
    RectF btns(rs.X - 2 * K, rs.Y - 2 * K, rp.GetRight() - rs.X + 4 * K, rs.Height + 4 * K);
    std::wstring text =
        busy_ ? L"Читаю" + std::wstring((size_t)((GetTickCount64() - busySince_) / 350 % 4), L'.') : L"Текст";
    rise(0.22f, btns, [&](Graphics& gb) {
        fillButton(gb, rs, savedPath_.empty() ? L"Сохранить" : L"Папка", H_SAVE, saveFill_, false);
        fillButton(gb, rt, text, H_TEXT, textFill_, true);
        DrawButton(gb, rp, L"", false, H_PIN);
        // значок: кнопка-булавка, остриё вниз
        float cx = rp.X + rp.Width / 2, cy = rp.Y + rp.Height / 2;
        gb.SetSmoothingMode(SmoothingModeHighQuality);
        Pen pinPen(fg, 1.6f * K);
        pinPen.SetStartCap(LineCapRound);
        pinPen.SetEndCap(LineCapRound);
        gb.DrawLine(&pinPen, cx - 5 * K, cy - 7 * K, cx + 5 * K, cy - 7 * K);
        gb.DrawLine(&pinPen, cx - 3 * K, cy - 7 * K, cx - 3 * K, cy - 1 * K);
        gb.DrawLine(&pinPen, cx + 3 * K, cy - 7 * K, cx + 3 * K, cy - 1 * K);
        gb.DrawLine(&pinPen, cx - 6 * K, cy + 1 * K, cx + 6 * K, cy + 1 * K);
        gb.DrawLine(&pinPen, cx - 3 * K, cy - 1 * K, cx - 6 * K, cy + 1 * K);
        gb.DrawLine(&pinPen, cx + 3 * K, cy - 1 * K, cx + 6 * K, cy + 1 * K);
        gb.DrawLine(&pinPen, cx, cy + 1 * K, cx, cy + 8 * K);
    });
    if (StripH() > 0) {  // лента прошлых снимков
        for (size_t i = 0; i < recent_.size(); ++i) {
            int id = H_STRIP + (int)i;
            RectF r = R(id);
            GraphicsPath path;
            RoundRectPath(path, r, 7 * K);
            GraphicsState st = g.Save();
            g.SetClip(&path, CombineModeIntersect);
            if (recent_[i].thumb) {
                g.SetInterpolationMode(InterpolationModeBilinear);
                g.DrawImage(recent_[i].thumb.get(), r);
            }
            bool on = (int)i == current_;
            if (!on) {  // невыбранные приглушены и светлеют под курсором
                SolidBrush dim(Color((BYTE)(90 * (1 - HoverOf(id))), 0, 0, 0));
                g.FillRectangle(&dim, r);
            }
            g.Restore(st);
            Pen edge(on ? ac : Color(56, 255, 255, 255), (on ? 2.0f : 1.0f) * K);
            g.DrawPath(&edge, &path);
            if (recent_[i].keep) {  // отмеченный - маленькая звезда в углу
                PointF star[10];
                for (int s = 0; s < 10; ++s) {
                    float rad = (s % 2 ? 2.2f : 5.0f) * K, ang = (float)(kPi * s / 5 - kPi / 2);
                    star[s] = PointF(r.GetRight() - 7 * K + rad * (float)std::cos(ang), r.Y + 7 * K + rad * (float)std::sin(ang));
                }
                SolidBrush gold(Color(255, 255, 210, 63));
                g.FillPolygon(&gold, star, 10);
            }
        }
        DrawButton(g, R(H_COMPARE), L"Сравнить", false, H_COMPARE);
    }
    // звезда на превью: видна под курсором или если снимок уже отмечен
    const bool kept = current_ >= 0 && current_ < (int)recent_.size() && recent_[(size_t)current_].keep;
    if (kept || Hovered()) {
        RectF sr = R(H_STAR);
        SolidBrush back(Color((BYTE)(150 + 80 * HoverOf(H_STAR)), 15, 15, 20));
        g.FillEllipse(&back, sr);
        PointF star[10];
        float cx = sr.X + sr.Width / 2, cy = sr.Y + sr.Height / 2;
        for (int s = 0; s < 10; ++s) {
            float rad = (s % 2 ? 3.0f : 7.0f) * K, ang = (float)(kPi * s / 5 - kPi / 2);
            star[s] = PointF(cx + rad * (float)std::cos(ang), cy + rad * (float)std::sin(ang));
        }
        if (kept) {
            SolidBrush gold(Color(255, 255, 210, 63));
            g.FillPolygon(&gold, star, 10);
        } else {
            Pen outline(Color(230, 255, 255, 255), 1.2f * K);
            g.DrawPolygon(&outline, star, 10);
        }
    }
    if (jumpT_ >= 0) {  // капля от превью к кнопке, по дуге
        float t = std::min(1.0f, jumpT_), e = t * t * (3 - 2 * t), arc = (float)std::sin(kPi * t), sq = 1 + arc * 0.5f;
        float ax = pv.X + pv.Width / 2, ay = pv.Y + pv.Height / 2;
        float bx = rs.X + rs.Width / 2, by = rs.Y + rs.Height / 2;
        float x = ax + (bx - ax) * e, y = ay + (by - ay) * e - arc * 38 * K, rx = 7 * K * sq, ry = 7 * K / sq;
        SolidBrush db(ac);
        g.FillEllipse(&db, x - rx, y - ry, 2 * rx, 2 * ry);
    }
}

int SnipCard::HitContent(const PointF& p) const {
    for (int id : {H_SAVE, H_TEXT, H_PIN, H_STAR, H_PREVIEW})  // звезда лежит на превью: она раньше
        if (R(id).Contains(p)) return id;
    if (StripH() > 0) {
        if (R(H_COMPARE).Contains(p)) return H_COMPARE;
        for (int i = 0; i < (int)recent_.size(); ++i)
            if (R(H_STRIP + i).Contains(p)) return H_STRIP + i;
    }
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
    // Л29: понятное имя по окну («MyGame Player.cpp 2026-10-25 10-41.png») и папка проекта, если снимали из него
    std::wstring name = snipname::FileName(context_, t);
    std::wstring dir = targetDir_;
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        dir = RememberedFolder(L"");  // общая папка, если выбирали; иначе «Снимки экрана»
        if (dir.empty()) dir = SaveFolder();
    }
    std::wstring path = dir + L"\\" + name;
    for (int n = 2; n < 100 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n)  // уже есть - «(2)»
        path = dir + L"\\" + name.substr(0, name.size() - 4) + L" (" + std::to_wstring(n) + L").png";
    if (!dir.empty() && WriteFileBytes(path, png_)) {
        savedPath_ = path;
        std::wstring where = dir == targetDir_ && !targetLabel_.empty() ? L" в " + targetLabel_ : L"";
        status_ = L"Сохранено" + where + L": " + path.substr(dir.size() + 1);
        statusGood_ = true;
    } else {
        status_ = L"Не удалось сохранить";
        statusGood_ = false;
    }
    Hold(kShowMs);  // итог должен успеть прочитаться, сколько бы ни оставалось до ухода
    Invalidate();
}

void SnipCard::Click(int id) {
    if (id == H_SAVE) {
        if (!savedPath_.empty()) {
            Save(true);  // уже лежит: показать в папке
        } else if (fx::Game()) {
            Save(false);  // в игре без анимаций: сразу
        } else if (jumpT_ < 0 && !png_.empty()) {
            jumpT_ = 0;  // сохранит капля, когда долетит
            Hold(kShowMs);
        }
    }
    if (id == H_TEXT && !busy_ && !png_.empty()) {
        busy_ = true;
        busySince_ = GetTickCount64();
        status_ = L"Распознаю текст...";
        statusGood_ = true;
        Hold(0);  // не уходит, пока не придёт итог: иначе текст ляжет в буфер молча
        ocr::RecognizeAsync(png_, Wnd(), WM_OCR_DONE);
        Invalidate();
    }
    if (id == H_PREVIEW) OpenEditor();
    if (id == H_PIN) PinCurrent();
    if (id == H_STAR) ToggleKeep();
    if (id == H_COMPARE) CompareWith(current_ > 0 ? current_ - 1 : current_ + 1);
    if (id >= H_STRIP && id < H_STRIP + kRecent) Select(id - H_STRIP);
}

bool SnipCard::Message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        OnClipboard();
        return true;
    }
    // Превью можно унести мышью. Базовая карточка об этом не знает: для неё нажатие отменяется отпусканием мимо
    if (msg == WM_LBUTTONDOWN) {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        dragArmed_ = !png_.empty() && R(H_PREVIEW).Contains((REAL)p.x, (REAL)p.y) && !R(H_STAR).Contains((REAL)p.x, (REAL)p.y);
        dragFrom_ = p;
        return false;
    }
    if (msg == WM_LBUTTONUP) dragArmed_ = false;
    // Правый щелчок обычно открывает меню карточки; у двух мест он свой
    if (msg == WM_RBUTTONUP) {
        PointF p((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp));
        if (R(H_SAVE).Contains(p)) {
            ChooseFolder();
            return true;
        }
        if (StripH() > 0)
            for (int i = 0; i < (int)recent_.size(); ++i)
                if (R(H_STRIP + i).Contains(p)) {
                    CompareWith(i);
                    return true;
                }
    }
    if (msg == WM_MOUSEMOVE && dragArmed_ && (wp & MK_LBUTTON)) {
        int dx = GET_X_LPARAM(lp) - dragFrom_.x, dy = GET_Y_LPARAM(lp) - dragFrom_.y;
        if (std::hypot(dx, dy) > 6 * k()) {
            dragArmed_ = false;
            ReleaseCapture();
            StartDrag();
            SendMessageW(Wnd(), WM_LBUTTONUP, 0, MAKELPARAM(-1, -1));
        }
        return true;  // пока не решено, это щелчок или перенос, карточка не считает его броском вправо
    }
    if (msg != WM_OCR_DONE) return false;
    std::unique_ptr<std::wstring> text((std::wstring*)lp);
    if (ocrStale_ > 0) {  // снимок уже другой: чужой текст не должен затереть буфер обмена
        --ocrStale_;
        return true;
    }
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
    Hold(kShowMs);  // результат виден ещё несколько секунд
    flash_ = 1;
    Invalidate();
    return true;
}
