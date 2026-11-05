#pragma once
#include <functional>
#include <memory>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/themes.h"
#include "media/lyrics.h"
#include "media/media.h"

struct Config;

// Панель «Сейчас играет»: курсор задержался у нижнего края экрана - снизу выезжает карточка с пластинкой:
// обложка на её яблоке, рычаг звукоснимателя идёт к центру вместе с песней. Пауза - рычаг поднимается, пластинка
// останавливается по инерции; другой трек - она укатывается, приезжает новая; перемотка - царапает.
// Колесо над карточкой - громкость самого плеера. Кнопка «Текст» раскрывает карточку вверх
// с текстом песни. При смене трека снизу ненадолго выглядывает маленькая карточка, из неё вырастает большая.
class MediaWidget {
public:
    std::function<void()> onTurnOff;  // «Выключить панель» из меню по ПКМ

    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);  // включить, выключить, перекрасить
    // раз в 20 мс из общего опроса; blocked - сейчас нельзя (зажат Alt, открыто кольцо)
    void Tick(const Config& cfg, bool blocked);
    void Destroy();
    // для разработки: кадр карточки с этим треком на ровном фоне в PNG, без окна и без опроса
    struct SnapState {
        float morph = 1, rx = 0, arm = -8, ang = 30, expand = 0;
    };
    bool SnapFrame(const Config& cfg, const media::Track& t, const SnapState& st, double k, const std::wstring& out);
    // для разработки: настоящий такт и окно на выдуманных треках - открыть, пауза, другой трек, царапнуть, закрыть,
    // маленькая карточка, вырасти. Кадры и журнал состояния - в папку dir (окно на это время видно на экране)
    void DemoRun(HINSTANCE inst, const Config& cfg, const media::Track& a, const media::Track& b,
                 const std::wstring& dir);
    // для разработки: сколько мс занимает кадр, когда крутится только пластинка, и полная перерисовка
    void Bench(const Config& cfg, const media::Track& t, double k, double& spinMs, double& fullMs);

private:
    enum Hit { H_NONE, H_COVER, H_TEXT, H_PREV, H_PLAY, H_NEXT, H_BAR, H_LYR, H_LYRICS, H_SRC, H_BODY, kHits };
    enum Mode { M_FULL, M_MINI };

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnTrack();
    void FeedTrack(media::Track t);
    void ApplyTrack(const media::Track& t);
    void BeginSwap();
    void Close();
    float Rev(float delay, float dur) const;  // появление содержимого: 0..1
    Gdiplus::Color Ac() const;                // цвет обложки с плавным переходом
    Gdiplus::RectF RecordRect() const;
    void BuildDisc();
    void FreeFrame();
    void DrawStatic(Gdiplus::Graphics& g);
    void DrawDynamic(Gdiplus::Graphics& g);
    void DrawDisc(Gdiplus::Graphics& g, const Gdiplus::RectF& rc);
    void DrawLabel(Gdiplus::Graphics& g, const Gdiplus::RectF& rc);
    void Open(POINT pt, const RECT& mon);
    void Toast();
    void Render();
    void DrawFull(Gdiplus::Graphics& g, float alpha);
    void DrawMini(Gdiplus::Graphics& g, float alpha);
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
    COLORREF accent_ = 0, accentFrom_ = 0;  // из обложки, без неё - из темы; с чего идёт переход
    float acMix_ = 1;
    media::Watcher watch_;
    media::Track track_;
    lyrics::Fetcher fetch_;
    std::unique_ptr<Gdiplus::Bitmap> artBg_, note_;
    std::unique_ptr<Gdiplus::Bitmap> disc_, label_;  // пластинка (бороздки и блик неподвижны) и яблоко с обложкой
    bool hasArt_ = false;
    std::unique_ptr<Gdiplus::Bitmap> shadowFull_, shadowTall_, shadowMini_;
    uint32_t artId_ = 0, lastTrackId_ = 0;

    Mode mode_ = M_FULL;
    bool open_ = false;
    float slide_ = 0, slideV_ = 0;                   // 0 - спрятана под краем, 1 - выехала (пружина, может перелететь)
    float morph_ = 1, morphV_ = 0;                   // 0 - маленькая карточка, 1 - большая
    float rx_ = -160, rxV_ = 0, rxT_ = 0;            // сдвиг пластинки, dip: катится слева
    float arm_ = -22, armV_ = 0;                     // угол рычага, градусы: -22 снят, -14..6 на пластинке
    float ang_ = 0, spin_ = 0;                       // поворот яблока и скорость, градусов в секунду
    float eq_ = 0, glow_ = 1;                        // столбики и отсвет: тише на паузе
    float titleA_ = 1, titleOff_ = 0, titleW_ = -1;  // название: появление, прокрутка, ширина
    ULONGLONG lastTick_ = 0, needleAt_ = 0, revealAt_ = 0, closingSince_ = 0, titleOverSince_ = 0;
    // другой трек: карта уже пришла, но показываем её, когда старая пластинка укатится
    int swap_ = 0;
    ULONGLONG swapSince_ = 0;
    bool haveNext_ = false;
    media::Track next_;
    // кадр: неподвижный слой копируется, сверху рисуется только то, что движется
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    std::vector<uint32_t> layer_;
    bool layerDirty_ = true, discBaked_ = false;
    bool demoHold_ = false;  // DemoRun: курсор ни при чём, карточка держится сама
    bool lyricsOn_ = false;
    float expand_ = 0;  // раскрытие вверх под текст песни
    float lyrPos_ = 0;  // плавная прокрутка текста (номер строки в центре)
    float plainOff_ = 0;
    float hover_[kHits] = {};
    Hit hot_ = H_NONE, down_ = H_NONE;
    int hotSrc_ = -1, downSrc_ = -1;
    std::vector<Gdiplus::RectF> srcRects_;
    bool tracking_ = false;
    bool edgeHold_ = false;  // плеер уже запущен от края: следующий раз - когда курсор уйдёт и вернётся
    ULONGLONG edgeSince_ = 0, awaySince_ = 0, toastUntil_ = 0, miniOverSince_ = 0;
    POINT toastPt_{};  // где был курсор, когда выглянула маленькая карточка: она могла выехать прямо под него
    float vol_ = -1;   // показанная громкость после колеса
    bool volApp_ = true;
    ULONGLONG volUntil_ = 0;
    float volShow_ = 0;
    int lastSecond_ = -1;
    bool dirty_ = false;
    double k_ = 0;
    RECT mon_{};
    POINT pos_{};  // левый верхний угол окна, когда панель выехала
};
