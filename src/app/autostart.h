#pragma once

// Запуск вместе с Windows. Основной путь - задача планировщика MoonRadial\RadialMenuLogon
// «при входе» (стартует раньше программ из Run), запасной - значение в HKCU\...\Run.
bool AutostartOn();
bool AutostartEarly();  // включён через планировщик
void SetAutostart(bool on);
// Чего хочет пользователь (последнее включение или выключение): 1 - запускать, 0 - нет, -1 - ещё не знаем.
// Самовосстановление возвращает автозапуск, только если его хотели, а не включает сам.
int AutostartWanted();
void SetAutostartWanted(bool on);
// После старта: перенести запись из Run в планировщик и поправить путь, если exe переехал.
void AutostartRefresh();
