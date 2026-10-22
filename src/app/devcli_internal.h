#pragma once
// Служебные режимы запуска (devcli.cpp - разбор командной строки и мелкие режимы), разнесённые по темам:
// devcli_net.cpp - сеть и скорость, devcli_tools.cpp - утилиты, «Защита», слежка, MFT, devcli_snaps.cpp - снимки,
// фильмы сцен, эталоны и замер кольца. Общее - здесь.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

#include "core/config.h"

namespace devcli {

std::vector<std::wstring> Args();
bool Value(const std::vector<std::wstring>& args, const wchar_t* flag, std::wstring& out);
bool Has(const std::vector<std::wstring>& args, const wchar_t* flag);
void WriteOut(const wchar_t* name, const std::wstring& text);  // итог проверки - в файл рядом с exe
Config LoadOwnConfig();

// GDI+ на время одного режима.
struct Gdip {
    ULONG_PTR token = 0;
    Gdip() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    ~Gdip() { Gdiplus::GdiplusShutdown(token); }
};

// devcli_net.cpp
int NetDiag();
int SpeedDiag();
// devcli_tools.cpp
int ToolsDiag();
int SecDiag();
int WatchDiag(bool slow);
int SysDiag();
int WatchSelftest();
int MftTest(const std::wstring& dir);
// devcli_snaps.cpp
DWORD WINAPI FakeClean(void*);
int SceneFilm(const std::wstring& what, const std::wstring& out);
int SnapSuite(const std::wstring& refDir, const std::wstring& outDir, bool update);
int RingBench(int ms);

}  // namespace devcli
