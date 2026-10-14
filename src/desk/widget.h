#pragma once
#include <functional>
#include <memory>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/themes.h"
#include "desk/lyrics.h"
#include "desk/media.h"

struct Config;

// Панель «Сейчас играет»: курсор задержался у нижнего края экрана - снизу выезжает карточка
// с обложкой, названием, полосой времени и кнопками. Курсор ушёл - карточка уезжает обратно.
// Колесо над карточкой - громкость самого плеера. Кнопка «Текст» раскрывает карточку вверх
// с текстом песни. При смене трека снизу ненадолго выглядывает маленькая карточка.
class MediaWidget {
public:
    std::function<void()> onTurnOff;  // «Выключить панель» из меню по ПКМ

    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);  // включить, выключить, перекрасить
    // раз в 20 мс из общего опроса; blocked - сейчас нельзя (зажат Alt, открыто кольцо)
    void Tick(const Config& cfg, bool blocked);
    void Destroy();

private:
    enum Hit { H_NONE, H_COVER, H_TEXT, H_PREV, H_PLAY, H_NEXT, H_BAR, H_LYR, H_LYRICS, H_SRC, H_BODY, kHits };
    enum Mode { M_FULL, M_MINI };

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnTrack();
    void Open(POINT pt, const RECT& mon);
    void Toast();
    void Render();
    void DrawFull(Gdiplus::Graphics& g);
    void DrawMini(Gdiplus::Graphics& g);
    void DrawLyrics(Gdiplus::Graphics& g, const Gdiplus::RectF& area, float alpha);
    void DrawVolume(Gdiplus::Graphics& g);
    Hit HitAt(POINT client, int* src = nullptr) const;
    Gdiplus::RectF Rect(Hit h) const;  // в пикселях окна
    Gdiplus::RectF Body() const;
    SIZE WindowSize() const;
    void SetScale(double k);
    void SetArt();
    void ContextMenu();
    void RequestLyrics();
    void Wheel(int delta, POINT client);
    // «назад» доступно и без очереди: тогда оно перематывает в начало трека
    bool CanNext() const { return track_.canNext; }
    bool CanPrev() const { return track_.canPrev || (track_.canSeek && track_.end > 0); }

    HINSTANCE inst_ = nullptr;
    HWND wnd_ = nullptr;
    bool enabled_ = false, toastOn_ = true, skipFull_ = true;
    ThemeInfo theme_{};
    COLORREF accent_ = 0;  // из обложки, без неё - из темы
    media::Watcher watch_;
    media::Track track_;
    lyrics::Fetcher fetch_;
    std::unique_ptr<Gdiplus::Bitmap> art_, artSmall_, artBg_, note_;
    std::unique_ptr<Gdiplus::Bitmap> shadowFull_, shadowTall_, shadowMini_;
    uint32_t artId_ = 0, lastTrackId_ = 0;

    Mode mode_ = M_FULL;
    bool open_ = false;
    float slide_ = 0;  // 0 - спрятана под краем, 1 - выехала
    bool lyricsOn_ = false;
    float expand_ = 0;  // раскрытие вверх под текст песни
    float lyrPos_ = 0;  // плавная прокрутка текста (номер строки в центре)
    float plainOff_ = 0;
    float hover_[kHits] = {};
    Hit hot_ = H_NONE, down_ = H_NONE;
    int hotSrc_ = -1, downSrc_ = -1;
    std::vector<Gdiplus::RectF> srcRects_;
    bool tracking_ = false;
    ULONGLONG edgeSince_ = 0, awaySince_ = 0, toastUntil_ = 0;
    float vol_ = -1;  // показанная громкость после колеса
    bool volApp_ = true;
    ULONGLONG volUntil_ = 0;
    float volShow_ = 0;
    int lastSecond_ = -1;
    bool dirty_ = false;
    double k_ = 0;
    RECT mon_{};
    POINT pos_{};  // левый верхний угол окна, когда панель выехала
};
