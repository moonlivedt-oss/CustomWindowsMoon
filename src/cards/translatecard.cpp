#include "cards/translatecard.h"

#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "cards/toast.h"
#include "core/config.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "helpers/snip.h"
#include "helpers/translate.h"
#include "helpers/translate_logic.h"
#include "ring/selring.h"

using namespace Gdiplus;

namespace translatecard {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialTranslate";
constexpr UINT WM_TR_DONE = WM_APP + 41, WM_TR_STEP = WM_APP + 42, WM_TR_MODEL = WM_APP + 43;
constexpr float kMaxTextH = 15 * 19;  // длинный перевод обрезается многоточием: целиком его даст «Копировать»
constexpr int kShowMs = 12000;

class Card : public Toast {
public:
    void Make(HINSTANCE inst) { CreateWnd(inst, kClass, L"Перевод"); }
    void Theme(const Config& cfg) {
        theme_ = CurrentTheme(cfg);
        Invalidate();
    }

    void Ask(const std::wstring& text, std::function<void(std::wstring)> replace, bool forceGoogle = false) {
        source_ = text;
        replace_ = std::move(replace);
        Translate(forceGoogle);
    }

    void ShowText(const std::wstring& title, const std::wstring& text) {
        ++tag_;
        mode_ = Mode::Text;
        source_ = text;
        result_ = text;
        title_ = title;
        replace_ = nullptr;
        status_ = L"Текст уже в буфере обмена";
        Layout();
        Open(kShowMs * 2, true);
        Invalidate();
    }

    void ShowError(const std::wstring& title, const std::wstring& message) {
        ++tag_;
        mode_ = Mode::Error;
        title_ = title;
        status_ = message;
        source_.clear();
        Layout();
        Open(kShowMs, true);
        Invalidate();
    }

    void DownloadModel() {
        mode_ = Mode::Busy;
        title_ = L"Модель перевода";
        status_ = L"Скачиваю модель с серверов Argos, около 100 МБ. Текст никуда не отправляется";
        Layout();
        Open(0, true);
        Invalidate();
        translate::InstallModel(modelFrom_, translate::TargetFor(modelFrom_), ++tag_, Wnd(), WM_TR_MODEL);
    }

    bool Working() const { return mode_ == Mode::Busy; }
    bool Frame(const std::wstring& png) { return SaveFrame(png); }

private:
    enum class Mode { Busy, Result, Text, Error };
    enum { H_COPY = H_USER, H_REPLACE, H_GOOGLE, H_BROWSER, H_MODEL, H_TRANSLATE };

    void Translate(bool forceGoogle) {
        mode_ = Mode::Busy;
        title_ = L"Перевод";
        result_.clear();
        status_ = forceGoogle ? L"Отправляю текст в Google..." : L"Перевожу на этом компьютере...";
        Layout();
        Open(0, true);
        Invalidate();
        translate::Job job;
        job.texts = {source_};
        job.source = translate::SourceOf(source_);
        job.target = translate::TargetFor(job.source);
        job.forceGoogle = forceGoogle;
        job.tag = ++tag_;
        job.wnd = Wnd();
        job.msgDone = WM_TR_DONE;
        job.msgStep = WM_TR_STEP;
        modelFrom_ = job.source;
        translate::Start(std::move(job));
    }

    std::vector<std::pair<int, std::wstring>> Buttons() const {
        std::vector<std::pair<int, std::wstring>> out;
        switch (mode_) {
            case Mode::Result:
                out.push_back({H_COPY, L"Копировать"});
                if (replace_) out.push_back({H_REPLACE, L"Заменить"});
                if (via_ == translate::Via::Local && translate::GoogleAllowed())
                    out.push_back({H_GOOGLE, L"Через Google"});
                break;
            case Mode::Text:
                out.push_back({H_TRANSLATE, L"Перевести"});
                break;
            case Mode::Error:
                if (problem_ == translate::Problem::NoModel) out.push_back({H_MODEL, L"Скачать модель"});
                // похожее на секрет наружу не уходит: ни само, ни одним щелчком
                if (!source_.empty() && !translate::ContainsSecret(source_)) out.push_back({H_BROWSER, L"В браузере"});
                break;
            case Mode::Busy:
                break;
        }
        return out;
    }

    void Layout() {
        // выбор кнопки на смену содержимого не должен оставаться от прошлого: подсветка считается от номера
        Invalidate();
    }

    float BodyHeight(Graphics& g) override {
        float K = k(), tw = (kW - 32) * K;
        Font fS(Family(), 12 * K, FontStyleRegular, UnitPixel);
        FontFamily fam(L"Segoe UI");
        Font fT(&fam, 13.5f * K, FontStyleRegular, UnitPixel);
        float h = 16 + 21;
        if (!status_.empty()) h += 4 + TextHeight(g, status_, fS, tw) / K;
        if (!result_.empty()) h += 8 + std::min(TextHeight(g, result_, fT, tw) / K, kMaxTextH);
        if (!Buttons().empty()) h += 12 + 32;
        return h + 16;
    }

    RectF Btn(int index, int count) const {
        RectF body = Body();
        float K = k(), gap = 8 * K, w = (body.Width - 32 * K - gap * (count - 1)) / count;
        return RectF(body.X + 16 * K + index * (w + gap), body.GetBottom() - (16 + 32) * K, w, 32 * K);
    }

    void Paint(Graphics& g, const RectF& body) override {
        float K = k(), x = body.X + 16 * K, tw = body.Width - 32 * K;
        Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(theme_.ac);
        Font fH(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
        Font fS(Family(), 12 * K, FontStyleRegular, UnitPixel);
        FontFamily fam(L"Segoe UI");
        Font fT(&fam, 13.5f * K, FontStyleRegular, UnitPixel);
        SolidBrush tB(fg), mB(mode_ == Mode::Error ? Color(255, 240, 140, 120) : MixColor(muted, fg, 0.2));
        float y = body.Y + 15 * K;
        StringFormat one;
        one.SetFormatFlags(StringFormatFlagsNoWrap);
        one.SetTrimming(StringTrimmingEllipsisCharacter);
        g.DrawString(title_.c_str(), -1, &fH, RectF(x, y, tw - 40 * K, 22 * K), &one, &tB);
        y += 23 * K;
        if (!status_.empty()) {
            float h = TextHeight(g, status_, fS, tw);
            std::wstring say = status_;
            if (mode_ == Mode::Busy) say += std::wstring((size_t)(GetTickCount64() / 350 % 4), L'.');
            g.DrawString(say.c_str(), -1, &fS, RectF(x, y, tw, h + 2), nullptr, &mB);
            y += h + 4 * K;
        }
        if (!result_.empty()) {
            y += 4 * K;
            float h = std::min(TextHeight(g, result_, fT, tw), kMaxTextH * K);
            StringFormat wf;
            wf.SetTrimming(StringTrimmingEllipsisWord);
            g.DrawString(result_.c_str(), -1, &fT, RectF(x, y, tw, h), &wf, &tB);
        }
        auto buttons = Buttons();
        for (size_t i = 0; i < buttons.size(); ++i)
            DrawButton(g, Btn((int)i, (int)buttons.size()), buttons[i].second, i == 0, buttons[i].first);
        (void)ac;
    }

    int HitContent(const PointF& p) const override {
        auto buttons = Buttons();
        for (size_t i = 0; i < buttons.size(); ++i)
            if (Btn((int)i, (int)buttons.size()).Contains(p)) return buttons[i].first;
        return H_NONE;
    }

    void Click(int id) override {
        if (id == H_COPY && snipimg::TextToClipboard(Wnd(), result_)) {
            status_ = L"Скопировано";
            Invalidate();
        }
        if (id == H_REPLACE && replace_) {
            std::wstring text = result_;
            auto fn = replace_;
            Dismiss();
            fn(text);
        }
        if (id == H_GOOGLE) Translate(true);
        if (id == H_TRANSLATE) { Translate(false); }
        if (id == H_MODEL) DownloadModel();
        if (id == H_BROWSER) {
            // прежний путь: страница переводчика в браузере; текст уходит в Google и в историю браузера
            std::wstring url = L"https://translate.google.com/?sl=auto&tl=" +
                               translate::TargetFor(translate::SourceOf(source_)) + L"&op=translate&text=" +
                               selring::UrlEncode(source_.substr(0, 4000));
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Dismiss();
        }
    }

    bool Busy() const override { return mode_ == Mode::Busy; }

    bool Message(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_TR_STEP) {
            if (mode_ != Mode::Busy) return true;
            if (wp == translate::kStarting)
                status_ = L"Запускаю переводчик. В первый раз это до 30 с";
            else if (lp > 1)
                status_ = L"Перевожу: " + std::to_wstring((int)wp + 1) + L" из " + std::to_wstring((int)lp);
            else
                status_ = L"Перевожу на этом компьютере";
            Invalidate();
            return true;
        }
        if (msg == WM_TR_MODEL) {
            std::unique_ptr<translate::Result> r((translate::Result*)lp);
            if (!r || r->tag != tag_) return true;
            if (r->ok && !source_.empty()) {
                Translate(false);
            } else {
                mode_ = r->ok ? Mode::Text : Mode::Error;
                problem_ = r->problem;
                title_ = L"Модель перевода";
                result_.clear();
                status_ = r->ok ? L"Модель скачана, можно переводить" : r->error;
                Hold(kShowMs);
                Invalidate();
            }
            return true;
        }
        if (msg != WM_TR_DONE) return false;
        std::unique_ptr<translate::Result> r((translate::Result*)lp);
        if (!r || r->tag != tag_) return true;  // пока ждали, попросили другое
        via_ = r->via;
        problem_ = r->problem;
        std::wstring pair = r->source + L" - " + r->target;
        if (r->ok && !r->texts.empty() && !Trim(r->texts[0]).empty()) {
            mode_ = Mode::Result;
            result_ = r->texts[0];
            title_ = L"Перевод " + pair;
            status_ = r->via == translate::Via::Google ? L"Через Google: текст уходил на его серверы"
                                                       : L"На этом компьютере, без сети";
            Hold(0);  // читать можно сколько нужно: закроется крестиком
        } else {
            mode_ = Mode::Error;
            result_.clear();
            title_ = L"Перевод " + pair;
            status_ = r->error.empty() ? L"Перевод не получился" : r->error;
            if (problem_ == translate::Problem::NoModel)
                status_ +=
                    L". Скачать её - около 100 МБ, ваш текст при этом не отправляется. Или открыть переводчик в "
                    L"браузере: текст уйдёт в Google";
            else if (problem_ == translate::Problem::NoLocal)
                status_ += L". Можно открыть переводчик в браузере: текст уйдёт в Google";
            Hold(0);
        }
        Invalidate();
        return true;
    }

    Mode mode_ = Mode::Error;
    uint64_t tag_ = 0;
    std::wstring source_, result_, title_, status_, modelFrom_ = L"ru";
    std::function<void(std::wstring)> replace_;
    translate::Via via_ = translate::Via::Local;
    translate::Problem problem_ = translate::Problem::None;
};

Card g_card;

}  // namespace

void Create(HINSTANCE inst) { g_card.Make(inst); }
void Apply(const Config& cfg) {
    g_card.Theme(cfg);
    translate::SetGoogleAllowed(cfg.translateGoogle);
}
void Tick() { g_card.Tick(); }
void Destroy() {
    translate::Stop();
    g_card.Destroy();
}
void Ask(const std::wstring& text, std::function<void(std::wstring)> replace) { g_card.Ask(text, std::move(replace)); }
void ShowText(const std::wstring& title, const std::wstring& text) { g_card.ShowText(title, text); }
void ShowError(const std::wstring& title, const std::wstring& message) { g_card.ShowError(title, message); }
void DownloadModel() { g_card.DownloadModel(); }
bool DemoBusy() { return g_card.Working(); }
bool DemoFrame(const std::wstring& png) { return g_card.Frame(png); }

}  // namespace translatecard
