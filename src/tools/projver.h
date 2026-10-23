#pragma once
// Окружение проекта (Л33): какую версию Node, Python и CMake проект просит (.nvmrc, .node-version, engines в
// package.json; .python-version, requires-python в pyproject.toml; cmake_minimum_required), какая первой в PATH и
// какие стоят на компьютере. Не та первая, а нужная стоит - подсказать и открыть терминал проекта с ней первой
// (TermEnv из DetectEnv получает её папку в начало PATH). projver_logic.cpp - разбор и выбор (под тестами);
// projver.cpp - поиск установленных версий.
#include <string>
#include <vector>

namespace tools {

struct Ver {
    int major = 0, minor = -1;  // minor -1 - любой
    bool Known() const { return major > 0; }
};
struct Need {
    Ver v;
    bool atLeast = false;  // «>=3.11»: подходит эта и новее; иначе - та же ветка (major, и minor, если задан)
    std::wstring from;     // откуда взято: «.nvmrc», «engines», «pyproject.toml»
};
struct Installed {
    Ver v;
    std::wstring dir;  // папка с exe
};

Ver ParseVer(const std::wstring& s);  // «v20.11.0» -> 20.11, «3.12» -> 3.12, «lts/iron» -> неизвестно
bool Fits(const Need& n, const Ver& have);
// Что просит проект: тексты файлов (пусто - файла нет)
Need NodeNeed(const std::wstring& nvmrc, const std::wstring& nodeVersion, const std::wstring& packageJson);
Need PythonNeed(const std::wstring& pythonVersion, const std::wstring& pyproject);
Need CMakeNeed(const std::wstring& cmakeLists);
// Лучшая подходящая из установленных: точная ветка, иначе самая новая подходящая; -1 - ни одна
int Pick(const Need& n, const std::vector<Installed>& have);

// --- на компьютере ---
struct EnvPick {
    std::wstring tool;  // «Node», «Python», «CMake»
    Need need;
    Ver first;      // какая сейчас первая в PATH (неизвестно - не нашлась)
    Installed use;  // какую подставить
};
// Что в проекте dir не совпадает с первой в PATH и есть чем заменить
std::vector<EnvPick> ProjectPicks(const std::wstring& dir);
std::wstring VerText(const Ver& v);  // «20», «3.12»

}  // namespace tools
