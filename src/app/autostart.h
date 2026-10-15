#pragma once

// Запуск вместе с Windows. Основной путь - задача планировщика MoonDesk\RadialMenuStart
// «при входе» (стартует раньше программ из Run), запасной - значение в HKCU\...\Run.
bool AutostartOn();
bool AutostartEarly();  // включён через планировщик
void SetAutostart(bool on);
// После старта: перенести запись из Run в планировщик и поправить путь, если exe переехал.
void AutostartRefresh();
