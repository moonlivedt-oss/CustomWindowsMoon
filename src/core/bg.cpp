#include "core/bg.h"

#include <atomic>
#include <memory>

namespace bg {

namespace {

std::atomic<int> g_running{0};
std::atomic<bool> g_quit{false};

struct Task {
    std::function<void()> fn;
    int priority;
};

DWORD WINAPI Thread(void* p) {
    std::unique_ptr<Task> t((Task*)p);
    if (t->priority != THREAD_PRIORITY_NORMAL) SetThreadPriority(GetCurrentThread(), t->priority);
    t->fn();
    --g_running;
    return 0;
}

}  // namespace

bool Run(std::function<void()> fn, int priority) {
    auto* t = new Task{std::move(fn), priority};
    ++g_running;
    HANDLE th = CreateThread(nullptr, 0, Thread, t, 0, nullptr);
    if (!th) {
        --g_running;
        delete t;
        return false;
    }
    CloseHandle(th);
    return true;
}

int Running() { return g_running; }

bool Quitting() { return g_quit; }

void Quit(DWORD ms) {
    g_quit = true;
    for (ULONGLONG until = GetTickCount64() + ms; g_running > 0 && GetTickCount64() < until;) Sleep(20);
}

}  // namespace bg
