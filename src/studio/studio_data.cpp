// Данные студии: отмена и возврат, сохранение, копии, проверка путей, действия над секторами.
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "core/files.h"
#include "core/perf.h"
#include "app/autostart.h"
#include "core/text.h"
#include "gfx/accent.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

Item* Studio::Sel() {
    auto& r = Ring(ring_);
    if (sel_ < 0 || sel_ >= (int)r.size()) return nullptr;
    return &r[sel_];
}

void Studio::PushUndo() {
    redo_.clear();
    undo_.push_back(cfg);
    if (undo_.size() > 60) undo_.erase(undo_.begin());
}

void Studio::Change() {
    PushUndo();
    editingField_ = 0;
}

// Набор текста в одном поле - одна отмена, а не по букве.
void Studio::FieldEdit(int fid) {
    if (editingField_ != fid) {
        PushUndo();
        editingField_ = fid;
    }
    savePending_ = true;
    SetTimer(wnd, kSaveTimer, 450, nullptr);
}

void Studio::Save() {
    PERF_SCOPE("studio.Save");
    KillTimer(wnd, kSaveTimer);
    savePending_ = false;
    Backup();
    if (!SaveConfig(cfgPath, cfg)) {
        Log(L"Не удалось записать radial.cfg");
        return;
    }
    stats_.clear();
    listings_.clear();  // пути могли поменяться
    Recheck();
    status_ = L"Сохранено автоматически";
    statusFresh_ = true;
    if (onSaved) onSaved();
    ui.Invalidate();
}

void Studio::Undo() {
    if (undo_.empty()) return;
    redo_.push_back(cfg);
    cfg = undo_.back();
    undo_.pop_back();
    editingField_ = 0;
    toastUntil_ = 0;
    for (auto* r : {&cfg.left, &cfg.right})
        for (Item& it : *r) Resolve(it);
    sel_ = std::clamp(sel_, 0, std::max(0, (int)Ring(ring_).size() - 1));
    ApplyAccent();
    Log(L"Последнее изменение отменено");
    Save();
}

void Studio::Redo() {
    if (redo_.empty()) return;
    undo_.push_back(cfg);
    cfg = redo_.back();
    redo_.pop_back();
    editingField_ = 0;
    for (auto* r : {&cfg.left, &cfg.right})
        for (Item& it : *r) Resolve(it);
    sel_ = std::clamp(sel_, 0, std::max(0, (int)Ring(ring_).size() - 1));
    ApplyAccent();
    Log(L"Изменение возвращено");
    Save();
}

void Studio::SectorOp(int op) {
    auto& ring = Ring(ring_);
    Item* it = Sel();
    if (!it) return;
    if (op == OP_RENAME) {
        renaming_ = sel_;
        renameFocus_ = true;
        return;
    }
    if ((op == OP_UP && sel_ == 0) || (op == OP_DOWN && sel_ + 1 >= (int)ring.size())) return;
    Change();
    std::wstring name = it->name;
    if (op == OP_UP) {
        std::swap(ring[sel_], ring[sel_ - 1]);
        --sel_;
    } else if (op == OP_DOWN) {
        std::swap(ring[sel_], ring[sel_ + 1]);
        ++sel_;
    } else if (op == OP_OTHER_SIDE) {
        Ring(1 - ring_).push_back(*it);
        ring.erase(ring.begin() + sel_);
        sel_ = std::clamp(sel_, 0, std::max(0, (int)ring.size() - 1));
        Toast(L"«" + name + L"» перенесён на " + (ring_ == 0 ? L"правый край" : L"левый край"));
    } else if (op == OP_DUPLICATE) {
        Item copy = *it;
        copy.name += L" (копия)";
        ring.insert(ring.begin() + sel_ + 1, copy);
        ++sel_;
        Log(L"Сектор «" + name + L"» продублирован");
    } else if (op == OP_DELETE) {
        ring.erase(ring.begin() + sel_);
        sel_ = std::clamp(sel_, 0, std::max(0, (int)ring.size() - 1));
        Toast(L"Сектор «" + name + L"» удалён");
    }
    Save();
}

std::vector<std::wstring> Studio::Backups() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ExeDir() + L"\\backups\\radial-*.cfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.rbegin(), out.rend());  // имена с датой: новые первыми
    return out;
}

// Копия перед сохранением: в начале сеанса студии и потом не чаще раза в 10 минут, хранятся 10 последних.
void Studio::Backup() {
    ULONGLONG now = GetTickCount64();
    if (lastBackup_ && now - lastBackup_ < 10 * 60 * 1000) return;
    if (!Exists(cfgPath)) return;
    lastBackup_ = now;
    std::wstring dir = ExeDir() + L"\\backups";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"radial-%04d-%02d-%02d_%02d-%02d-%02d.cfg", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CopyFileW(cfgPath.c_str(), (dir + L"\\" + name).c_str(), FALSE);
    auto all = Backups();
    for (size_t i = 10; i < all.size(); ++i) DeleteFileW((dir + L"\\" + all[i]).c_str());
}

void Studio::Restore(const std::wstring& file) {
    Config c;
    if (!LoadConfig(ExeDir() + L"\\backups\\" + file, c)) {
        Log(L"Не удалось прочитать " + file);
        return;
    }
    Change();
    c.tourDone = true;
    cfg = c;
    ApplyAccent();
    sel_ = 0;
    Toast(L"Возвращена копия " + file.substr(7, 16));
    Save();
}

void Studio::Log(const std::wstring& s) {
    wchar_t t[16];
    time_t now = time(nullptr);
    tm lt{};
    localtime_s(&lt, &now);
    swprintf(t, 16, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    log_.push_back({t, s});
    if (log_.size() > 200) log_.erase(log_.begin());
    status_ = s;
    statusFresh_ = true;
    ui.Invalidate();
}

void Studio::ApplyAccent() {
    COLORREF c;
    cfg.accent = cfg.accentAuto && WallpaperAccent(c) ? c : cfg.accentManual;
}

// Пути, которых больше нет: папку переместили или удалили.
void Studio::Recheck() {
    PERF_SCOPE("studio.Recheck");
    problems_.clear();
    auto check = [](const std::wstring& path) {
        if (path.empty() || path.rfind(L"shell:", 0) == 0 || path.find(L"://") != std::wstring::npos) return true;
        return Exists(ExpandPath(path));
    };
    for (int r = 0; r < 2; ++r) {
        auto& ring = Ring(r);
        for (int i = 0; i < (int)ring.size(); ++i) {
            const Item& it = ring[i];
            if (!check(it.path)) problems_.push_back({r, i, -1, it.path, 0});
            else if (it.path.empty() && !it.HasRing()) problems_.push_back({r, i, -1, it.path, 1});
            else if (it.expand && !it.path.empty() && !it.dir && it.path.rfind(L"shell:", 0) != 0)
                problems_.push_back({r, i, -1, it.path, 2});
            for (int j = 0; j < (int)it.children.size(); ++j) {
                const Item& ch = it.children[j];
                if (!check(ch.path)) problems_.push_back({r, i, j, ch.path, 0});
                else if (ch.path.empty()) problems_.push_back({r, i, j, ch.path, 1});
            }
        }
    }
}

// Ищем файл или папку с тем же именем: выше по пути и на рабочем столе, до 3 уровней вниз.
void Studio::FixProblem(const Problem& p) {
    std::wstring full = ExpandPath(p.path);
    while (!full.empty() && full.back() == L'\\') full.pop_back();
    std::wstring name = PathFindFileNameW(full.c_str());
    std::vector<std::wstring> roots;
    std::wstring up = full;
    while (true) {
        size_t at = up.find_last_of(L'\\');
        if (at == std::wstring::npos) break;
        up = up.substr(0, at);
        if (IsFolder(up)) {
            roots.push_back(up);
            break;
        }
    }
    roots.push_back(L"D:\\Desktop");
    roots.push_back(ExpandPath(L"{desktop}"));
    std::wstring found;
    std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
        if (!found.empty() || depth > 3) return;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> subs;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || n[0] == L'.' || n == L"node_modules") continue;
            if (_wcsicmp(n.c_str(), name.c_str()) == 0) {
                found = dir + L"\\" + n;
                break;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subs.push_back(dir + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto& s : subs) walk(s, depth + 1);
    };
    for (auto& r : roots) walk(r, 0);
    if (found.empty()) {
        Log(L"«" + name + L"» не нашлась, укажи путь вручную");
        Defer([this, p] {
            std::wstring pick = PickPath(true);
            if (pick.empty()) return;
            Change();
            Item& it = p.j < 0 ? Ring(p.ring)[p.i] : Ring(p.ring)[p.i].children[p.j];
            it.path = pick;
            Resolve(it);
            Log(L"Путь исправлен: " + pick);
            Save();
        });
        return;
    }
    Change();
    Item& it = p.j < 0 ? Ring(p.ring)[p.i] : Ring(p.ring)[p.i].children[p.j];
    it.path = found;
    Resolve(it);
    Log(L"Нашлась «" + name + L"»: " + found);
    Save();
}

const std::vector<Item>& Studio::Listing(const Item& it) {
    std::wstring key = it.full + (it.files ? L"|f" : L"|");
    auto f = listings_.find(key);
    if (f == listings_.end()) f = listings_.emplace(key, ListFolder(it.full, it.files)).first;
    return f->second;
}

FolderStats Studio::Stats(const Item& it) {
    if (!it.dir) return {};
    std::wstring key = it.full + (it.files ? L"|f" : L"|");
    auto f = stats_.find(key);
    if (f != stats_.end()) return f->second;
    FolderStats s;
    for (const Item& c : Listing(it)) {
        ++s.total;
        if (c.dir) ++s.dirs;
    }
    stats_[key] = s;
    return s;
}

std::wstring Studio::IconFile(const Item& it) const {
    if (!it.iconFull.empty()) return it.iconFull;
    if (it.dir && !cfg.folderIconFull.empty()) return cfg.folderIconFull;
    return L"";
}

void Studio::Defer(std::function<void()> f) {
    deferred_.push_back(std::move(f));
    PostMessageW(wnd, WM_RUN, 0, 0);
}

std::wstring Studio::PickPath(bool folder) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    std::wstring out;
    if (SUCCEEDED(d->Show(wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = p;
            CoTaskMemFree(p);
            it->Release();
        }
    }
    d->Release();
    return out;
}

bool Studio::PickColor(COLORREF& c) {
    static COLORREF custom[16] = {RGB(255, 122, 107), RGB(232, 96, 140), RGB(255, 211, 110), RGB(31, 17, 41)};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = wnd;
    cc.rgbResult = c;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    c = cc.rgbResult;
    return true;
}

void Studio::AddPaths(const std::vector<std::wstring>& paths, bool asChildren) {
    if (paths.empty()) return;
    Change();
    auto& r = Ring(ring_);
    for (const auto& p : paths) {
        Item it;
        it.path = p;
        std::wstring n = PathFindFileNameW(p.c_str());
        if (!IsFolder(p)) {
            PathRemoveExtensionW(n.data());
            n.resize(wcslen(n.c_str()));
        }
        it.name = n;
        Resolve(it);
        if (asChildren && Sel()) {
            Sel()->children.push_back(it);
        } else {
            it.expand = it.dir;
            r.push_back(it);
            sel_ = (int)r.size() - 1;
        }
    }
    Log(asChildren ? L"Пункты добавлены: " + std::to_wstring(paths.size())
                   : L"Секторы добавлены: " + std::to_wstring(paths.size()));
    Save();
}

Bitmap* Studio::Img(const std::wstring& path, int maxSide, const std::wstring& tag) {
    std::wstring key = std::to_wstring(maxSide) + L"|" + path + tag;
    auto it = imgs_.find(key);
    if (it != imgs_.end()) return it->second.get();
    auto b = LoadImageFile(path, maxSide);
    Bitmap* raw = b.get();
    imgs_[key] = std::move(b);
    return raw;
}

}  // namespace studio
