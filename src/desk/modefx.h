#pragma once
#include <string>
#include <vector>

#include <windows.h>

#include "core/themes.h"

// Анимация включения и выключения режимов поверх всего экрана (окно прозрачно для мыши, фокус не берёт).
// Игра, включение: внизу заряжается сотовое ядро, взрывается ударной волной и осколками, по краям экрана
//   пробегает волна сот, ядро раскрывается в плашку с названием, над ней всплывают чипы - что сделано.
// Игра, выключение: края гаснут от верха к середине низа, ядро остывает из цвета темы в сталь,
//   от плашки поднимаются и гаснут искры, чипы - что вернули.
// Unreal, включение: снизу вверх проходит луч и оставляет сетку чертежа, по углам встают рамки видоискателя,
//   от плашки к чипам-узлам протягиваются провода с бегущими импульсами, как в блюпринтах.
// Unreal, выключение: провода втягиваются, узлы схлопываются, луч сверху стирает сетку.
// В эксклюзивном полноэкранном режиме не показывается.
class ModeFx {
public:
    enum Kind { GAME, DEV };
    void Create(HINSTANCE inst);
    void Destroy();
    void Play(Kind kind, bool on, const std::wstring& title, const std::vector<std::wstring>& chips,
              const ThemeInfo& theme);
    // для проверки без экрана: лист кадров в PNG (--modefx-film)
    static int Film(Kind kind, bool on, const std::wstring& title, const std::vector<std::wstring>& chips,
                    const ThemeInfo& theme, const std::wstring& out);

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void Frame();
    void Draw(void* bits, int W, int H, double k, double el) const;
    void Present(int W, int H);
    void Stop();
    double Total() const;

    HWND wnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    RECT mon_{};
    ThemeInfo theme_{};
    Kind kind_ = GAME;
    bool on_ = true;
    std::wstring title_;
    std::vector<std::wstring> chips_;
    ULONGLONG t0_ = 0;
};
