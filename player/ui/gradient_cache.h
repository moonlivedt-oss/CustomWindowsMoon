#pragma once
// Кэш коллекций градиента. Коллекция - это текстура на видеокарте: создавать её на каждом кадре
// дорого, а цвета большинства градиентов не меняются от кадра к кадру.
// Ключ - сами цвета и настройки, поэтому места вызова остаются прежними. Коллекции принадлежат
// устройству: при его смене кэш очищают (Clear).

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace ui {

class GradientCache {
public:
    // Обычная коллекция, как ID2D1RenderTarget::CreateGradientStopCollection
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> Get(ID2D1DeviceContext* dc, const D2D1_GRADIENT_STOP* stops,
                                                            UINT32 count, D2D1_GAMMA gamma = D2D1_GAMMA_2_2,
                                                            D2D1_EXTEND_MODE extend = D2D1_EXTEND_MODE_CLAMP) {
        Key(stops, count, static_cast<int>(gamma), static_cast<int>(extend));
        if (const auto it = items_.find(key_); it != items_.end()) return it->second;
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> made;
        dc->CreateGradientStopCollection(stops, count, gamma, extend, &made);
        return Remember(made);
    }

    // Смешивание в premultiplied, как в CSS: «цвет -> transparent» не уходит в серый
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> GetPremultiplied(ID2D1DeviceContext* dc,
                                                                         const D2D1_GRADIENT_STOP* stops,
                                                                         UINT32 count) {
        Key(stops, count, -1, -1);
        if (const auto it = items_.find(key_); it != items_.end()) return it->second;
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection1> precise;
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> made;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, count, D2D1_COLOR_SPACE_SRGB, D2D1_COLOR_SPACE_SRGB,
                                                       D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_EXTEND_MODE_CLAMP,
                                                       D2D1_COLOR_INTERPOLATION_MODE_PREMULTIPLIED, &precise)))
            made = precise;
        else  // запасной путь: реализации D2D без этого метода
            dc->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &made);
        return Remember(made);
    }

    void Clear() { items_.clear(); }

private:
    void Key(const D2D1_GRADIENT_STOP* stops, UINT32 count, int a, int b) {
        const size_t bytes = count * sizeof(D2D1_GRADIENT_STOP);
        key_.resize(bytes + sizeof(a) + sizeof(b));
        std::memcpy(key_.data(), stops, bytes);
        std::memcpy(key_.data() + bytes, &a, sizeof(a));
        std::memcpy(key_.data() + bytes + sizeof(a), &b, sizeof(b));
    }

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> Remember(
        const Microsoft::WRL::ComPtr<ID2D1GradientStopCollection>& made) {
        // Плавные смены цвета дают новые ключи: кэш не должен расти без конца
        if (items_.size() >= kLimit) items_.clear();
        if (made) items_.emplace(key_, made);
        return made;
    }

    static constexpr size_t kLimit = 256;
    std::string key_;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID2D1GradientStopCollection>> items_;
};

}  // namespace ui
