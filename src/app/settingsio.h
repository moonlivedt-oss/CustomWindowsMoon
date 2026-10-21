#pragma once
// Перенос настроек (И29): всё своё одним файлом .moonradial - radial.cfg, профили Boost, «Это моё» в «Защите»,
// Minecraft. Импорт сначала кладёт нынешние файлы в backups\import-<время>, потом пишет новые. Пути в radial.cfg -
// как были на старом ПК: чего нет, кольцо покажет серым, правится в студии.
// Формат: строка «MOONRADIAL-SETTINGS 1», потом записи «@имя<TAB>байт\n» + сами байты + «\n».
#include <string>
#include <vector>

namespace settingsio {

struct Part {
    std::wstring name;  // логическое имя: radial.cfg, data/boost_profiles.tsv, tools/ignored.tsv...
    std::string bytes;
};

std::string Pack(const std::vector<Part>& parts);
bool Unpack(const std::string& blob, std::vector<Part>& out);  // false - не наш файл или битый

// Сами файлы этого ПК: имя -> путь. Нет файла - пропускается.
std::vector<std::pair<std::wstring, std::wstring>> Files();
bool Export(const std::wstring& file, std::wstring& msg);
bool Import(const std::wstring& file, std::wstring& msg);  // после - перечитать настройки

}  // namespace settingsio
