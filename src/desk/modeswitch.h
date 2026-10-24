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
