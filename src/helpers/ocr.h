#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "helpers/ocrtext_logic.h"

// Распознавание текста на картинке встроенным OCR Windows (тот же, что у AutoTranslator).
// Языки - из профиля пользователя (русский и английский у тебя). Вызывать из потока MTA:
// работа занимает до секунды, поэтому из окна её запускают в отдельном потоке (RecognizeAsync ниже).
namespace ocr {

// png - картинка как есть (PNG/BMP/JPG). Текст для копирования: перенесённые строки склеены в абзацы, код и таблицы
// остаются строками (ocrtext::CleanText). Мелкое перед распознаванием увеличивается. Пусто - не нашлось.
std::wstring Recognize(const std::vector<uint8_t>& png, std::wstring* error = nullptr);

// Распознать в своём потоке и прислать окну msg: lParam = new std::wstring (окно удаляет).
void RecognizeAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg);

// Строки с положением на исходной картинке, оба движка уже сверены
using Line = ocrtext::Line;
bool RecognizeLines(const std::vector<uint8_t>& png, std::vector<Line>& out, std::wstring* error = nullptr);
// Прислать окну msg: lParam = new std::vector<Line> (окно удаляет), wParam 0 - успех
void RecognizeLinesAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg);
// Для сравнения в разработке (--ocr ... raw): без подготовки картинки
void SetEnhance(bool on);

// Слова с положением на картинке: чтобы показать, где на снимке похожее на секрет. Читают оба движка (основной и
// английский): латиницу основной часто искажает. Строки разных движков нумеруются порознь.
struct Word {
    std::wstring text;
    float x = 0, y = 0, w = 0, h = 0;
    int line = 0;
};
bool RecognizeWords(const std::vector<uint8_t>& png, std::vector<Word>& out, std::wstring* error = nullptr);
// Прислать окну msg: lParam = new std::vector<Word> (окно удаляет), wParam 0 - успех
void RecognizeWordsAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg);

}  // namespace ocr
