#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "cards/toast.h"

struct Config;

// Картинка из буфера обмена (CF_DIB) и в PNG - общее со историей буфера (helpers/cliphist).
namespace snipimg {
std::unique_ptr<Gdiplus::Bitmap> FromDib(const BYTE* dib, SIZE_T size);
std::vector<uint8_t> ToPng(Gdiplus::Bitmap* b);
std::unique_ptr<Gdiplus::Bitmap> FromPng(const std::vector<uint8_t>& png);
bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& data);
bool ToClipboard(HWND owner, Gdiplus::Bitmap* b);  // CF_DIB, как у «Ножниц»
bool TextToClipboard(HWND owner, const std::wstring& text);
// Для проверок окон: буфер обмена и реестр настоящего пользователя не трогаются, картинка и текст ложатся в файлы
void SetTestOutput(const std::wstring& file);
bool TestMode();
// Путь в своей временной папке: файлы для перетаскивания и просмотра. Чужой программе они нужны и после броска,
// поэтому сразу не удаляются: старые убирает PurgeTemp
std::wstring TempFile(const std::wstring& name);
void PurgeTemp(ULONGLONG olderThanMs);
}  // namespace snipimg

// Карточка после снимка области (Win+Shift+S или горячий угол): справа снизу выезжает превью
// с кнопками «Сохранить» и «Текст»; клик по превью открывает снимок. «Текст» распознаёт снимок встроенным OCR
// Windows и кладёт текст в буфер обмена. Снимок замечается по буферу обмена: картинку туда
// положил «Ножницы» (ScreenClippingHost / SnippingTool). Стопка и движение - в Toast.
class SnipCard : public Toast {
public:
    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);
    void Destroy();
    void Demo();                              // для проверки: карточка со снимком кусочка экрана, как после «Ножниц»
    void DemoSave(const std::wstring& dir);   // для снимков карточки: нажать «Сохранить», файл ляжет в dir
    void DemoClose() { Dismiss(); }           // для снимков карточки: убрать, как крестиком
    void DemoBusy();                          // для снимков карточки: как во время распознавания, без OCR
    void DemoShot(int w, int h, bool saved);  // для снимков карточки: выдуманная картинка вместо экрана

private:
    // H_STRIP.. - лента, до H_M_LATER из Toast; звезда и сравнение - уже за номерами меню
    enum { H_PREVIEW = H_USER, H_SAVE, H_TEXT, H_PIN, H_STRIP, H_STAR = 28, H_COMPARE = 29 };
    static constexpr int kRecent = 5;

    // Прошлые снимки ленты живут только в памяти и забываются через полчаса: на снимке может быть личное.
    // Данные текущего лежат в полях карточки (png_, savedPath_...), в ленте у него только миниатюра
    struct Recent {
        uint64_t serial = 0;
        std::vector<uint8_t> png;
        std::unique_ptr<Gdiplus::Bitmap> thumb;
        int w = 0, h = 0;
        std::wstring savedPath, context, targetDir, targetLabel;
        bool keep = false;  // со звездой: живёт, пока программа запущена
    };
    void ToggleKeep();
    void CompareWith(int index);  // текущий и снимок из ленты в окне сравнения
    void ChooseFolder();          // правый щелчок по «Сохранить»
    std::vector<uint8_t> PngOf(int index) const;
    std::wstring RememberedFolder(const std::wstring& label) const;
    std::map<std::wstring, std::wstring> testFolders_;  // в проверках окон реестр не трогаем
    void Stash();            // текущий - обратно в ленту
    void Select(int index);  // показать снимок из ленты
    void MakeThumbs();       // миниатюры из shot_; большая картинка после этого не держится в памяти
    void OpenEditor();
    void ApplyEdit(uint64_t serial, std::unique_ptr<Gdiplus::Bitmap> image, bool save);
    void PinCurrent();
    void StartDrag();
    void Forget();
    float StripH() const { return recent_.size() >= 2 ? 44.0f : 0.0f; }
    std::vector<Recent> recent_;
    int current_ = -1;
    uint64_t nextSerial_ = 1;
    ULONGLONG lastShotAt_ = 0;
    UiColors uiColors_{};
    bool dragArmed_ = false;
    POINT dragFrom_{};
    bool editing_ = false;

    void TakeShot(std::unique_ptr<Gdiplus::Bitmap> shot);
    bool Message(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnClipboard();
    void Show();
    void Save(bool thenOpen);
    std::wstring SaveFolder() const;
    void FindTarget();  // Л29: из какого окна сняли - папка проекта и понятное имя

    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override { return busy_; }
    bool LiquidMotion() const override { return true; }
    bool HasMenu() const override { return !busy_; }
    void MenuChoose(int minutes, bool repeat) override;
    void Idle(ULONGLONG now) override;
    bool Animate(double dtMs) override;
    Gdiplus::RectF R(int id) const;

    bool enabled_ = true;
    std::unique_ptr<Gdiplus::Bitmap> shot_, thumb_;
    std::vector<uint8_t> png_;
    int shotW_ = 0, shotH_ = 0;
    float pw_ = 136, ph_ = 84;  // картинка в рамке превью, dip
    float saveFill_ = 1, textFill_ = 0;  // заливка кнопок: перетекает из «Сохранить» в «Текст»
    float jumpT_ = -1;                   // капля от превью к «Сохранить»: 0..1, -1 - нет
    float statusA_ = 1, flash_ = 0;      // появление итога и вспышка рамки превью
    std::wstring lastStatus_;
    ULONGLONG laterAt_ = 0;  // отложили: снова показать в это время (снимок лежит здесь же)
    int ocrStale_ = 0;          // сколько распознаваний ещё придут от уже заменённых снимков
    std::wstring status_, savedPath_;
    std::wstring context_;      // «MyGame Player.cpp» - откуда снимок, для имени файла
    std::wstring targetDir_;    // папка проекта\screenshots или папка Проводника; пусто - «Снимки экрана»
    std::wstring targetLabel_;  // как назвать её в карточке: имя проекта или папки
    bool busy_ = false, statusGood_ = true;
    ULONGLONG busySince_ = 0;
};
