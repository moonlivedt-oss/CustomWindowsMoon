#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Распознавание текста на картинке встроенным OCR Windows (тот же, что у AutoTranslator).
// Языки - из профиля пользователя (русский и английский у тебя). Вызывать из потока MTA:
// работа занимает до секунды, поэтому из окна её запускают в отдельном потоке (RecognizeAsync ниже).
namespace ocr {

// png - картинка как есть (PNG/BMP/JPG). Строки текста через перевод строки; пусто - не нашлось.
std::wstring Recognize(const std::vector<uint8_t>& png, std::wstring* error = nullptr);

// Распознать в своём потоке и прислать окну msg: lParam = new std::wstring (окно удаляет).
void RecognizeAsync(std::vector<uint8_t> png, void* hwnd, unsigned msg);

}  // namespace ocr
