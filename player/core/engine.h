#pragma once
// Аудиодвижок: устройство вывода miniaudio, голоса, громкость, плавные пуск и пауза.
//
// Кто чем владеет:
// - Поток UI вызывает все публичные методы, кроме CopyRecentMono (его тоже UI, но только чтение).
// - Аудиопоток (колбэк miniaudio) владеет голосами и декодерами, пока устройство запущено.
//   Команды к нему идут через SPSC-очередь (без замков), ответы - через атомарные поля
//   и очередь отработавших декодеров, которые освобождает поток UI в Service().
// - Пока устройство остановлено (пауза), колбэк не вызывается, и голосами владеет поток UI:
//   команды применяются сразу.
// - На паузе устройство останавливается после короткого затухания: в покое аудиопоток не крутится.
//
// Голосов два: второй держит заранее открытый следующий трек (бесшовный переход), потом -
// и кроссфейд. В цепочке голоса есть место для обработки (сохранение высоты тона при смене скорости).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/decoder.h"

namespace pm {

// Равномощный переход, t от 0 до 1: уходящий трек cos, входящий sin. Сумма квадратов 1 - громкость в середине
// перехода не проседает, как при линейном
float CrossfadeOut(float t);
float CrossfadeIn(float t);

class Engine {
public:
    static constexpr uint32_t kChannels = 2;

    struct Options {
        bool nullBackend = false;  // устройство-заглушка без звука (тесты, CI)
        uint32_t sampleRate = 0;   // 0 - родная частота устройства
    };
    // Вызывается из аудиопотока, когда потоку UI пора вызвать Service(). Должна быть потокобезопасной.
    using WakeFn = std::function<void()>;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Start(const Options& options, WakeFn wake, std::string* error = nullptr);
    void Shutdown();
    uint32_t SampleRate() const;

    // ----- поток UI -----
    // gain - множитель громкости трека (ReplayGain), 1 - без изменений.
    // Новый трек отменяет поставленный в очередь следующий.
    void SetTrack(std::unique_ptr<IDecoder> decoder, uint64_t startFrame, float gain = 1.f);
    // Бесшовный переход: следующий трек открыт заранее и начнётся в том же буфере, где кончился текущий.
    // crossfade - разрешён ли для этой пары плавный переход (внутри одного альбома - нет, там нужен стык)
    void QueueNext(std::unique_ptr<IDecoder> decoder, float gain = 1.f, bool crossfade = false);
    // Длина плавного перехода между треками, 0 - выключен. Действует со следующего перехода
    void SetCrossfade(double seconds);
    void ClearNext();
    void ClearTrack();
    void Play();
    void Pause();
    void Seek(uint64_t frame);
    void SetVolume(float volume);  // 0..1, применяется плавно (без щелчков)
    // Караоке: 0 - как записано, 1 - голос из центра убран (басы остаются). Плавно, без щелчков
    void SetKaraoke(float amount);

    bool IsPlaying() const;      // намерение: нажат ли «пуск»
    bool DeviceRunning() const;  // работает ли аудиопоток (на паузе после затухания - нет)
    bool HasTrack() const;
    uint64_t PositionFrames() const;
    uint64_t LengthFrames() const;

    // Обслуживание в потоке UI (по сигналу wake и при обновлении окна): освобождает отработавшие
    // декодеры, останавливает устройство после затухания паузы. true - текущий трек доиграл.
    bool Service();
    // Был ли с прошлого вызова бесшовный переход на трек из QueueNext
    bool TakeSwitched();

    // Последние n моно-сэмплов (для визуализатора). Возвращает сколько скопировано.
    size_t CopyRecentMono(float* dst, size_t n) const;
    uint64_t TapWritten() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm
