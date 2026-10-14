#pragma once
#include <windows.h>

// Замеры для разработки. Включаются переменной окружения RADIAL_PROFILE=1:
// при выходе программы в profile.log рядом с exe пишется сводка (сколько раз, среднее, максимум).
// Без переменной стоимость - одна проверка флага.
namespace perf {

bool Enabled();
void Record(const char* name, double ms);
void Flush();

class Scope {
public:
    explicit Scope(const char* name) : name_(Enabled() ? name : nullptr) {
        if (name_) QueryPerformanceCounter(&t0_);
    }
    ~Scope() {
        if (!name_) return;
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        Record(name_, (t1.QuadPart - t0_.QuadPart) * 1000.0 / f.QuadPart);
    }

private:
    const char* name_;
    LARGE_INTEGER t0_{};
};

}  // namespace perf

#define PERF_CAT2(a, b) a##b
#define PERF_CAT(a, b) PERF_CAT2(a, b)
#define PERF_SCOPE(name) perf::Scope PERF_CAT(perfScope_, __LINE__)(name)
