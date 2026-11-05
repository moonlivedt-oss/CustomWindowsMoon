#pragma once
// Карточка перевода справа снизу: кольцо выделенного просит перевести - здесь итог с кнопками «Копировать» и
// «Заменить». Сюда же приходит текст, распознанный с картинки из истории буфера. Фокус у программы не отнимает, поэтому
// «Заменить» попадает туда, где выделяли. Что куда уходит - helpers/translate.
#include <windows.h>

#include <functional>
#include <string>

struct Config;

namespace translatecard {

void Create(HINSTANCE inst);
void Apply(const Config& cfg);
void Tick();
void Destroy();

// Перевести text. replace - заменить выделенное в программе; пусто - заменять негде (текст из буфера)
void Ask(const std::wstring& text, std::function<void(std::wstring)> replace = nullptr);
// Показать готовый текст (распознанный с картинки): он уже в буфере обмена, отсюда его можно перевести
void ShowText(const std::wstring& title, const std::wstring& text);
// Показать сообщение об ошибке
void ShowError(const std::wstring& title, const std::wstring& message);
// Скачать модель для перевода с русского на английский: кнопка настроек
void DownloadModel();

// Для проверок: идёт ли работа и сохранить кадр карточки
bool DemoBusy();
bool DemoFrame(const std::wstring& png);

}  // namespace translatecard
