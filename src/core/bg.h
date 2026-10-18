#pragma once
// Разовые фоновые задачи одной строкой вместо CreateThread + new Box + delete: лямбда в своём потоке.
// Долгие потоки со своим циклом и событием остановки (слежка, медиа, яркость) живут сами по себе.
#include <windows.h>

#include <functional>

namespace bg {

// false - поток не создался, fn не вызвана (тогда вызывающий сам снимает свои флаги «занято»)
bool Run(std::function<void()> fn, int priority = THREAD_PRIORITY_NORMAL);
int Running();        // сколько задач ещё идёт
bool Quitting();      // программа закрывается: долгой задаче пора бросить работу
void Quit(DWORD ms);  // при выходе: отметить Quitting и подождать задачи не дольше ms

}  // namespace bg
