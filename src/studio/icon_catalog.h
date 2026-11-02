#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

// Local semantic catalog: no disk traversal, network request or model in the picker.
namespace studio::icon_catalog {
struct Entry {
    std::wstring label, tags;
    int topic = 0;
};
inline std::wstring Normalize(std::wstring s) {
    for (auto& c : s) {
        // Windows processes may still use the C locale: handle Cyrillic explicitly.
        if (c >= L'А' && c <= L'Я')
            c += L'а' - L'А';
        else if (c == L'Ё')
            c = L'ё';
        else
            c = (wchar_t)std::towlower(c);
        if (c == L'\\') c = L'/';
        if (c == L'ё') c = L'е';
    }
    return s;
}
inline bool Has(const std::wstring& s, const std::wstring& tokens) {
    size_t a = 0;
    while (a < tokens.size()) {
        size_t b = tokens.find(L'|', a);
        auto token = tokens.substr(a, b == std::wstring::npos ? b : b - a);
        if (!token.empty() && s.find(token) != std::wstring::npos) return true;
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return false;
}
inline const std::vector<std::wstring>& Topics() {
    static const std::vector<std::wstring> labels = {L"Подходящие", L"Все",   L"Разработка", L"Учёба",   L"Игры",
                                                     L"Музыка",     L"Файлы", L"Сеть",       L"Система", L"Карьера"};
    return labels;
}
inline const std::vector<std::wstring>& Keywords() {
    static const std::vector<std::wstring> words = {
        L"",
        L"",
        L"код|code|dev|разработ|проект|project|cpp|python|java|git|terminal|терминал|editor",
        L"учеб|учеба|study|курс|course|книг|book|lesson|конспект|обуч|docs|документ",
        L"игр|game|steam|unreal|unity|godot|play|minecraft",
        L"музык|music|audio|звук|плейлист|playlist|подкаст",
        L"файл|file|folder|папк|download|загруз|архив|archive|портфолио|portfolio|desktop|рабочий стол",
        L"сеть|сети|сетев|net|web|интернет|браузер|browser|wifi|dns|proxy|vpn|tunnel|router|ping",
        L"систем|system|tools|утилит|настрой|защит|clean|recycle|mem|rec-|диск|disk|shield|phone|телефон",
        L"карьер|career|contest|конкурс|соревнов|работа|resume|резюме|трофе|trophy"};
    return words;
}
inline int Topic(const std::wstring& context) {
    auto s = Normalize(context);
    for (int i : {5, 4, 3, 7, 9, 2, 8, 6})
        if (Has(s, Keywords()[i])) return i;
    return 0;
}
inline Entry Describe(const std::wstring& file) {
    auto path = Normalize(file);
    auto name = path.substr(path.find_last_of(L'/') + 1);
    auto stem = name.substr(0, name.find_last_of(L'.'));
    static const std::vector<std::pair<std::wstring, Entry>> known = {
        {L"projects", {L"Проекты", L"навигация орбита проекты projects разработка", 2}},
        {L"code", {L"Код", L"код разработка code dev скобки", 2}},
        {L"study", {L"Учёба", L"учеба книга справочник study book курс", 3}},
        {L"game", {L"Игры", L"игры game контроллер геймпад steam", 4}},
        {L"music", {L"Музыка", L"музыка звук music audio камертон", 5}},
        {L"apps", {L"Программы", L"программы apps application кубы запуск", 8}},
        {L"folder", {L"Папки", L"папки файлы folder file архив", 6}},
        {L"downloads", {L"Загрузки", L"загрузки download прием файлы", 6}},
        {L"web", {L"Интернет", L"интернет сеть web net globe браузер", 7}},
        {L"terminal", {L"Терминал", L"терминал командная строка terminal console код", 2}},
        {L"career", {L"Карьера", L"карьера конкурс career contest кубок часы работа", 9}},
        {L"tools", {L"Инструменты", L"инструменты tools система ремонт утилиты", 8}}};
    for (const auto& e : known)
        if (stem == e.first) return e.second;
    auto pretty = stem;
    std::replace(pretty.begin(), pretty.end(), L'-', L' ');
    return {pretty, pretty + L" " + path, Topic(stem)};
}
inline bool Matches(const Entry& e, const std::wstring& file, const std::wstring& query) {
    auto text = Normalize(e.label + L" " + e.tags + L" " + file);
    auto q = Normalize(query);
    size_t a = 0;
    while (a < q.size()) {
        auto b = q.find(L' ', a);
        auto word = q.substr(a, b == std::wstring::npos ? b : b - a);
        if (!word.empty() && text.find(word) == std::wstring::npos) return false;
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return true;
}
inline int Score(const Entry& e, const std::wstring& file, const std::wstring& context) {
    int topic = Topic(context);
    int score = topic && e.topic == topic ? 100 : 0;
    auto s = Normalize(context), path = Normalize(file);
    if (s.find(Normalize(e.label)) != std::wstring::npos) score += 40;
    auto name = path.substr(path.find_last_of(L'/') + 1);
    if (name.rfind(L"proj-", 0) == 0) {
        auto project = name.substr(5, name.size() - 9);
        if (!project.empty() && s.find(project) != std::wstring::npos) score += 250;
    }
    if (!topic && (name == L"folder.png" || name == L"apps.png" || name == L"projects.png")) score += 10;
    return score;
}
}  // namespace studio::icon_catalog
