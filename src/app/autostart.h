#pragma once

// Запуск вместе с Windows: значение в HKCU\...\Run.
bool AutostartOn();
void SetAutostart(bool on);
// При запуске: если автозапуск включён, а exe лежит по другому пути - обновить путь.
void AutostartRefresh();
