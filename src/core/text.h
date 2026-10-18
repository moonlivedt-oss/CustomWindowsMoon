#pragma once
#include <string>
#include <vector>

// Строки: UTF-8 <-> UTF-16, регистр, обрезка, окончания, русские числительные.
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);
std::wstring Trim(const std::wstring& s);
std::wstring Lower(std::wstring s);
bool EndsWithI(const std::wstring& s, const std::wstring& suf);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);  // части уже обрезаны
std::wstring Plural(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many);  // 1 пункт, 2 пункта, 5 пунктов
