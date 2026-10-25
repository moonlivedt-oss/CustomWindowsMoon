#pragma once
// Когда режиму (игровому, разработки) быть включённым: по тому, видна ли игра или редактор, и по ручному
// переключателю. Чистое, под тестами.
//
// Включается сразу, а выключается с запасом: игра или редактор должны пропасть на leaveMs подряд. Лаунчер
// передаёт игру exe-шнику, игра перезапускается после обновления, список процессов на миг теряет её - без запаса
// режим дёргался, и каждый раз гасли и загорались анимации Windows, прозрачность, обои, карточки итога сессии.
// «Выключить вручную» посреди игры держится, пока игра не пропадёт на тот же срок - один пропущенный опрос его
// больше не снимает.
#include <windows.h>

#include <cwchar>
#include <string>

// Окно на весь монитор, но точно не игра: браузеры, плееры, показ слайдов, удалённый стол, свои и рабочие
// программы. Имя exe - в нижнем регистре.
inline bool NotAGame(const std::wstring& exeLower) {
    static const wchar_t* kExact[] = {L"chrome.exe",
                                      L"msedge.exe",
                                      L"firefox.exe",
                                      L"opera.exe",
                                      L"brave.exe",
                                      L"vivaldi.exe",
                                      L"browser.exe",
                                      L"arc.exe",
                                      L"librewolf.exe",
                                      L"waterfox.exe",
                                      L"iexplore.exe",
                                      L"vlc.exe",
                                      L"mpc-hc64.exe",
                                      L"mpc-hc.exe",
                                      L"mpc-be64.exe",
                                      L"mpc-be.exe",
                                      L"mpv.exe",
                                      L"wmplayer.exe",
                                      L"video.ui.exe",
                                      L"smplayer.exe",
                                      L"stremio.exe",
                                      L"microsoft.media.player.exe",
                                      L"powerpnt.exe",
                                      L"acrord32.exe",
                                      L"acrobat.exe",
                                      L"explorer.exe",
                                      L"applicationframehost.exe",
                                      L"mstsc.exe",
                                      L"msrdc.exe",
                                      L"vmconnect.exe",
                                      L"obs64.exe",
                                      L"code.exe",
                                      L"devenv.exe",
                                      L"rider64.exe",
                                      L"blender.exe",
                                      L"discord.exe",
                                      L"telegram.exe",
                                      L"spotify.exe",
                                      L"steam.exe",
                                      L"steamwebhelper.exe",
                                      L"radialmenu.exe",
                                      L"radialshutup.exe",
                                      L"windowsterminal.exe",
                                      L"photos.exe",
                                      L"microsoft.photos.exe",
                                      L"unrealeditor.exe",
                                      L"ue4editor.exe"};
    static const wchar_t* kPrefix[] = {L"potplayer", L"kmplayer", L"vmware", L"virtualbox", L"plex"};
    for (const wchar_t* e : kExact)
        if (exeLower == e) return true;
    for (const wchar_t* p : kPrefix)
        if (exeLower.compare(0, wcslen(p), p) == 0) return true;
    return false;
}

// Библиотека, через которую рисуют игры (имя в нижнем регистре).
inline bool Is3dModule(const std::wstring& moduleLower) {
    return moduleLower == L"d3d9.dll" || moduleLower == L"d3d11.dll" || moduleLower == L"d3d12.dll" ||
           moduleLower == L"vulkan-1.dll" || moduleLower == L"opengl32.dll";
}

struct ModeSwitch {
    ULONGLONG leaveMs = 10000;
    bool on = false, manual = false, suppress = false;
    ULONGLONG lastSeen = 0;  // когда игру (редактор) видели последний раз; 0 - не видели

    // Очередной опрос: detected - игра видна сейчас. Возвращает, каким режиму быть.
    bool Want(ULONGLONG now, bool detected) {
        if (detected) lastSeen = now;
        bool gone = !detected && (!lastSeen || now - lastSeen >= leaveMs);
        if (gone) suppress = false;  // игра закрылась: в следующий раз режим снова включится сам
        if (manual) return true;
        if (suppress) return false;
        if (detected) return true;
        return on && !gone;  // игру не видно, но запас ещё не вышел - режим держится
    }

    // Переключили руками: выключили посреди игры - не включать снова, пока она идёт.
    void SetManual(bool want, ULONGLONG now, bool detected) {
        manual = want;
        if (detected) lastSeen = now;
        suppress = !want && detected;
        if (!want && !detected) lastSeen = 0;  // игры уже нет: выключить сразу, без запаса
    }
};
