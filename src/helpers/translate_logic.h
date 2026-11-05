#pragma once
// Перевод без окон и сети (под тестами): обмен с локальным помощником, разбор ответа Google, куда переводить,
// что нельзя отправлять наружу. Сам перевод - helpers/translate.
#include <string>
#include <vector>

namespace translate {

// Строка обмена с помощником: перевод строки, табуляция и обратная косая в ней экранированы
std::wstring Escape(const std::wstring& s);
std::wstring Unescape(const std::wstring& s);

struct HelperLine {
    enum Kind { Other, Ready, Fail, Ok, Err, Done } kind = Other;
    std::wstring id, text;
};
HelperLine ParseHelperLine(const std::wstring& line);

// Откуда и куда: большинство букв кириллица - с русского на английский, иначе на русский
std::wstring SourceOf(const std::wstring& text);
std::wstring TargetFor(const std::wstring& source);
// pairs - «en>ru,de>ru» от помощника
bool HasPair(const std::wstring& pairs, const std::wstring& source, const std::wstring& target);

// Похоже на токен, ключ, пароль или номер карты где-нибудь в тексте: такое не уходит наружу
bool ContainsSecret(const std::wstring& text);

// Ответ translate.googleapis.com: [[["перевод","оригинал",...],...],null,"en",...]
bool ParseGtx(const std::wstring& json, std::wstring& out);
// Длинный текст по кускам (у Google предел около 5000 знаков): рвём по абзацам, потом по предложениям и пробелам
std::vector<std::wstring> SplitChunks(const std::wstring& text, size_t limit = 4500);
// Ответ на склейку через перевод строки: сошлось число строк - true
bool SplitBatch(const std::wstring& joined, size_t count, std::vector<std::wstring>& out);
std::wstring HttpError(int status);

// Перевод ничего не изменил (имя, адрес, число): закрывать оригинал незачем
bool SameText(const std::wstring& a, const std::wstring& b);

}  // namespace translate
