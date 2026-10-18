#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Картинка любого формата, который знает WIC (PNG, JPG, WebP...), уменьшенная так,
// чтобы большая сторона была не больше maxSide (0 - без уменьшения). PARGB.
std::unique_ptr<Gdiplus::Bitmap> LoadImageFile(const std::wstring& path, int maxSide);

// Значки, заранее уменьшенные до нужного размера, чтобы кадр рисовался быстро.
// Значок оболочки (ярлык, exe, папка без своей картинки) достаётся десятки миллисекунд, а то и сотни: его грузит
// свой поток, а Get до этого отвечает nullptr. Arrived() скажет, что пора перерисовать кадр.
class IconCache {
public:
    IconCache() = default;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    ~IconCache();
    // png - картинка; если её нет или она не читается, берётся значок оболочки для shellPath.
    Gdiplus::Bitmap* Get(const std::wstring& png, const std::wstring& shellPath, int px);
    void Clear();
    bool Arrived();  // готовые значки из потока забраны в кэш (были новые - true)
    bool Idle();     // поток ничего не ждёт и не грузит (для замера)
    bool Waiting() const { return !pending_.empty(); }  // что-то ещё грузится в потоке

private:
    // Очередь общая с потоком и живёт, пока жив хоть один из них: студия удаляет свой кэш при закрытии,
    // а поток в это время может ещё доставать значок.
    struct Shared;
    static DWORD WINAPI Worker(void* shared);
    void Take();  // забрать готовое из очереди в кэш

    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> cache_;
    std::set<std::wstring> pending_;  // ждут потока
    std::shared_ptr<Shared> sh_;
};
