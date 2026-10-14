#pragma once
#include <string>

// Громкость: общая (устройство вывода по умолчанию) и отдельно у программы-плеера.
// Уровни 0..1; -1 - не получилось (нет устройства, плеер не звучит).
namespace vol {

float Master();
float AddMaster(float delta);
// Громкость сеанса плеера по его AppUserModelId (из медиа-сеанса).
float App(const std::wstring& aumid);
float AddApp(const std::wstring& aumid, float delta);

}  // namespace vol
