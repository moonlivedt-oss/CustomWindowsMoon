#pragma once
#include <string>

#include "desk/toast.h"
#include "gfx/scene.h"

// Карточка справа снизу с живой сценой вместо текста: очистка (вихрь) или проверка «Защиты» (щит).
// Сцена читает свою ленту (scene::Clean / scene::Guard); пока идёт работа - карточка не уходит,
// после итога висит несколько секунд. Одна на всю программу: SceneCard::Get().
class SceneCard : public Toast {
public:
    enum Kind { CLEAN, GUARD };
    static SceneCard& Get();
    // показать сцену; title - заголовок над ней. Студия на переднем плане сама показывает ту же сцену -
    // тогда карточка не нужна
    void Show(Kind kind, const std::wstring& title, const ThemeInfo& theme);
    void Poll();  // из общего опроса: работа кончилась - карточке пора уходить по таймеру

private:
    enum { H_SCENE = H_USER };
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override { return moving_; }
    Gdiplus::RectF SceneRect(const Gdiplus::RectF& body) const;

    Kind kind_ = CLEAN;
    std::wstring title_;
    scene::CleanScene clean_;
    scene::GuardScene guard_;
    bool moving_ = false, click_ = false, created_ = false;
    int doneGen_ = -1;
    size_t finds_ = 0;
};
