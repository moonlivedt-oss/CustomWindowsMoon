#pragma once

// Короткие звуки интерфейса, синтезируются при запуске, без файлов.
namespace Sound {
enum Kind { Open, Hover, Select, Back, Drop, Portal, Count };
void Init(int volume);  // 0..100
void Play(Kind k);
}  // namespace Sound
