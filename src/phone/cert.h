#pragma once
// «Телефон рядом», HTTPS без сторонних служб: свой корневой сертификат (создаётся на ПК один раз, закрытый ключ -
// под DPAPI в файле) и им подписанный сертификат сервера на нынешние адреса ПК. Телефон один раз ставит корневой
// («Сертификат ЦС» в настройках Android) - и Chrome доверяет странице по https://<IP>:<порт>/.
#include <windows.h>
#include <wincrypt.h>

#include <cstdint>
#include <string>
#include <vector>

namespace phone::cert {

// корневой из dir (phone-ca.cer и phone-ca.key) или новый, если их нет или они испорчены; der - сам сертификат
bool LoadOrCreateCa(const std::wstring& dir, std::string& der);
// сертификат сервера на эти IPv4 (порядок хоста), подписанный корневым из dir; закрытый ключ - в хранилище ключей
// пользователя под именем keyName (Schannel берёт его оттуда). Освобождать CertFreeCertificateContext. nullptr - нет
PCCERT_CONTEXT IssueServer(const std::wstring& dir, const std::vector<uint32_t>& ips, const std::wstring& keyName);
void DeleteKey(const std::wstring& keyName);  // для тестов
std::wstring DefaultDir();                    // %APPDATA%\MoonDesk

}  // namespace phone::cert
