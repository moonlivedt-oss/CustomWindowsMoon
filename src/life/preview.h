#pragma once
// Предпросмотр (Ф8): действие preview - выделенное в верхнем окне Проводника открывается большим окном: картинка
// целиком (GDI+/WIC), текст и код - первые 64 КБ моноширинным, видео, PDF и остальное - большая миниатюра Windows
// (кадр видео, первая страница PDF - то, что рисует сам Проводник) с именем и размером. Стрелки - соседние
// выделенные файлы (или файлы папки, если выделен один), Enter - открыть, Esc, пробел или щелчок мимо - закрыть.
// preview_logic.cpp - тип файла, подпись, вписывание (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace preview {

void Init(HINSTANCE inst, const Config* cfg);
void Show();  // взять выделенное в Проводнике
void ShowFiles(const std::vector<std::wstring>& files, size_t at);
void Shutdown();
inline void Tick() {}

// --- чистое ---
enum Kind { IMAGE, TEXT, OTHER };
Kind KindOf(const std::wstring& path);
// вписать w x h в box (не увеличивая больше чем в 2 раза), по центру
RECT Fit(int w, int h, const RECT& box);
std::wstring Caption(const std::wstring& name, long long bytes, int w, int h, size_t at, size_t count);

}  // namespace preview
