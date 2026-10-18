#pragma once
// Буфер без секретов: похоже ли скопированное на токен, ключ, пароль или номер карты. Такое не попадает в
// историю буфера и через минуту стирается из самого буфера. Только разбор текста - под тестами.
#include <string>

namespace secrets {

// пусто - не секрет; иначе что это: «номер карты», «токен GitHub», «закрытый ключ», «пароль»...
std::wstring Kind(const std::wstring& text);
bool Luhn(const std::wstring& digits);  // контрольная сумма номера карты (только цифры)
double Entropy(const std::wstring& s);  // бит на символ (Шеннон)

}  // namespace secrets
