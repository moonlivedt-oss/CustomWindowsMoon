#pragma once
// Конвертер броском (Ф2): файлы бросили на пункт action:convert («Превратить») - кольцо того, что с ними можно:
// картинки (webp, heic, avif, bmp, png, jpg...) - в PNG, в JPG, сжать до 8 МБ; видео - в MP4, в GIF (первые 15 с,
// 480 px), сжать до 8 или 25 МБ (под Discord); два PDF и больше - склеить в один. Картинки - через WIC (кодеки
// Windows), видео - через ffmpeg (если есть: PATH или winget), PDF - через pypdf (py -3). Результат ложится рядом с
// исходником с новым именем, ничего не перезаписывается; ход и итог - карточкой («Показать»). Всё локально.
// convert_logic.cpp - типы, имена, команды ffmpeg (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;
struct Item;
class RadialMenu;

namespace convert {

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu);
void Drop(const std::vector<std::wstring>& files);  // показать кольцо (чуть позже - пока гаснет прежнее)
void Tick();
void Shutdown();
std::vector<Item> RingItems(const std::vector<std::wstring>& files);
std::wstring RunSync(const std::wstring& key, const std::vector<std::wstring>& files);  // --convert
std::wstring FfmpegPath(const wchar_t* exe);  // ffmpeg.exe / ffprobe.exe: PATH или пакет winget; пусто - нет

// --- чистое ---
enum Kind { OTHER, IMAGE, VIDEO, PDF };
Kind KindOf(const std::wstring& path);
// «D:\a\clip.mov» + «.mp4» + «8 МБ» -> «D:\a\clip (8 МБ).mp4»; пустой tag - «D:\a\clip.mp4»
std::wstring OutName(const std::wstring& path, const std::wstring& ext, const std::wstring& tag);
// битрейт видео (кбит/с), чтобы файл влез в targetBytes при длительности sec (звук 96 кбит/с, запас 6%)
int VideoKbps(long long targetBytes, double sec);
// аргументы ffmpeg (без пути к exe): op - mp4 | gif | fit (сжать: kbps > 0)
std::wstring FfmpegArgs(const std::wstring& op, const std::wstring& in, const std::wstring& out, int kbps);

}  // namespace convert
