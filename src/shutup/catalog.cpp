// Каталог RadialShutUp: настройки, их состояние в реестре, правка с бэкапом и откат.
#include "shutup/catalog.h"

#include <algorithm>

#include "core/text.h"

namespace shutup {

namespace {

const HKEY CU = HKEY_CURRENT_USER, LM = HKEY_LOCAL_MACHINE;
constexpr Level R = Level::Recommended, L = Level::Limited, N = Level::NotRecommended;

RegValue D(HKEY root, const wchar_t* key, const wchar_t* name, DWORD v) { return {root, key, name, REG_DWORD, v, L""}; }
RegValue S(HKEY root, const wchar_t* key, const wchar_t* name, const wchar_t* v) {
    return {root, key, name, REG_SZ, 0, v};
}

constexpr wchar_t kCdm[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
constexpr wchar_t kAdv[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kSearch[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\SearchSettings";
constexpr wchar_t kData[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection";
constexpr wchar_t kCompat[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppCompat";
constexpr wchar_t kSys[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\System";
constexpr wchar_t kWSearch[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search";
constexpr wchar_t kAi[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsAI";
constexpr wchar_t kEdge[] = L"SOFTWARE\\Policies\\Microsoft\\Edge";
constexpr wchar_t kWu[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
constexpr wchar_t kBackup[] = L"Software\\RadialShutUp\\Backup";

// разрешение приложениям из Магазина: «Deny» вместо «Allow» (программы с рабочего стола - отдельный переключатель
// Windows, его не трогаем)
#define CONSENT(cap)                                                                                              \
    S(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\" cap, L"Value", \
      L"Deny")

const wchar_t* const kGroups[] = {
    L"Общая приватность",      L"Телеметрия и отчёты", L"Пуск, поиск и реклама", L"Разрешения приложений",
    L"Синхронизация и облако", L"Обновления и защита", L"Microsoft Edge",        L"Разное"};

}  // namespace

bool Setting::Machine() const {
    for (const RegValue& v : values)
        if (v.root == HKEY_LOCAL_MACHINE) return true;
    return false;
}

const std::vector<Setting>& Catalog() {
    static const std::vector<Setting> all = {
        // ---------------- общая приватность ----------------
        {L"ads_id",
         kGroups[0],
         L"Рекламный идентификатор",
         L"Программы из Магазина не получают общий номер для подбора рекламы.",
         R,
         {D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0)}},
        {L"ads_id_all",
         kGroups[0],
         L"Рекламный идентификатор для всех",
         L"То же для всех пользователей компьютера - политикой, поверх личной настройки.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", L"DisabledByGroupPolicy", 1)}},
        {L"lang_list",
         kGroups[0],
         L"Список языков для сайтов",
         L"Сайты не подбирают содержимое по списку ваших языков.",
         R,
         {D(CU, L"Control Panel\\International\\User Profile", L"HttpAcceptLanguageOptOut", 1)}},
        {L"tailored",
         kGroups[0],
         L"Персональные советы по диагностике",
         L"Microsoft не подбирает подсказки и рекламу по данным диагностики.",
         R,
         {D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy",
            L"TailoredExperiencesWithDiagnosticDataEnabled", 0)}},
        {L"typing",
         kGroups[0],
         L"Сбор набранного и рукописного текста",
         L"Словарь набора, почерка и контактов не собирается для «улучшения распознавания».",
         R,
         {D(CU, L"Software\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection", 1),
          D(CU, L"Software\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", 1),
          D(CU, L"Software\\Microsoft\\InputPersonalization\\TrainedDataStore", L"HarvestContacts", 0)}},
        {L"online_speech",
         kGroups[0],
         L"Распознавание речи в облаке",
         L"Голос не уходит на серверы Microsoft; диктовка и голосовой доступ работают локально, где умеют.",
         R,
         {D(CU, L"Software\\Microsoft\\Speech_OneCore\\Settings\\OnlineSpeechPrivacy", L"HasAccepted", 0)}},
        {L"activity",
         kGroups[0],
         L"История действий",
         L"Открытые файлы и сайты не копятся в истории и не уходят в облако.",
         R,
         {D(LM, kSys, L"PublishUserActivities", 0), D(LM, kSys, L"UploadUserActivities", 0),
          D(LM, kSys, L"EnableActivityFeed", 0)}},
        {L"handwriting",
         kGroups[0],
         L"Образцы почерка",
         L"Рукописный ввод и отчёты об ошибках его распознавания не отправляются в Microsoft.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\TabletPC", L"PreventHandwritingDataSharing", 1),
          D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\HandwritingErrorReports", L"PreventHandwritingErrorReports",
            1)}},
        {L"location",
         kGroups[0],
         L"Определение местоположения",
         L"Windows и программы не узнают, где вы. Перестанут работать погода по месту, карты, «Найти устройство».",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", L"DisableLocation", 1)}},
        {L"find_device",
         kGroups[0],
         L"«Поиск устройства»",
         L"Ноутбук не сообщает своё место в учётную запись Microsoft. Потерянный ноутбук так не найти.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\FindMyDevice", L"AllowFindMyDevice", 0)}},

        // ---------------- телеметрия и отчёты ----------------
        {L"telemetry",
         kGroups[1],
         L"Телеметрия - минимум",
         L"Данные диагностики - только обязательные. В Pro и Домашней меньше нельзя, 0 считается как 1.",
         R,
         {D(LM, kData, L"AllowTelemetry", 0)}},
        {L"device_name",
         kGroups[1],
         L"Имя компьютера в телеметрии",
         L"В отправляемой диагностике нет имени компьютера.",
         R,
         {D(LM, kData, L"AllowDeviceNameInTelemetry", 0)}},
        {L"diag_dumps",
         kGroups[1],
         L"Дампы и журналы в диагностике",
         L"В диагностику не попадают дампы памяти и подробные журналы.",
         R,
         {D(LM, kData, L"LimitDumpCollection", 1), D(LM, kData, L"LimitDiagnosticLogCollection", 1)}},
        {L"feedback",
         kGroups[1],
         L"Просьбы оставить отзыв",
         L"Windows не спрашивает «Насколько вероятно, что вы порекомендуете...».",
         R,
         {D(CU, L"Software\\Microsoft\\Siuf\\Rules", L"NumberOfSIUFInPeriod", 0),
          D(LM, kData, L"DoNotShowFeedbackNotifications", 1)}},
        {L"ceip",
         kGroups[1],
         L"Программа улучшения качества",
         L"Старый сбор статистики использования (CEIP) выключен.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows", L"CEIPEnable", 0)}},
        {L"inventory",
         kGroups[1],
         L"Опись программ",
         L"Windows не собирает и не отправляет список установленных программ и файлов.",
         R,
         {D(LM, kCompat, L"DisableInventory", 1), D(LM, kCompat, L"AITEnable", 0)}},
        {L"steps_rec",
         kGroups[1],
         L"Средство записи действий",
         L"Выключает запись шагов для отчётов о проблемах (сам Steps Recorder Windows уже убирает).",
         R,
         {D(LM, kCompat, L"DisableUAR", 1)}},
        {L"mrt_report",
         kGroups[1],
         L"Отчёты средства удаления вредоносных программ",
         L"Найденное ежемесячной проверкой MRT не отправляется в Microsoft. Сама проверка работает.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\MRT", L"DontReportInfectionInformation", 1)}},
        {L"wer",
         kGroups[1],
         L"Отчёты об ошибках Windows",
         L"Сведения о падениях программ не отправляются. Решения от Microsoft для таких падений тоже не придут.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Error Reporting", L"Disabled", 1)}},
        {L"diagtrack",
         kGroups[1],
         L"Служба телеметрии (DiagTrack)",
         L"Служба «Функциональные возможности для подключённых пользователей» не запускается. После перезагрузки.",
         L,
         {D(LM, L"SYSTEM\\CurrentControlSet\\Services\\DiagTrack", L"Start", 4)}},
        {L"dmwappush",
         kGroups[1],
         L"Служба WAP Push (dmwappushservice)",
         L"Передаёт сообщения управления устройством. Без неё может не работать вход в рабочие учётные записи "
         L"и часть синхронизации.",
         N,
         {D(LM, L"SYSTEM\\CurrentControlSet\\Services\\dmwappushservice", L"Start", 4)}},

        // ---------------- Пуск, поиск и реклама ----------------
        {L"start_ads",
         kGroups[2],
         L"Реклама и «советы» в Пуске и Параметрах",
         L"Рекомендуемые приложения, предложения в Параметрах, советы после обновлений.",
         R,
         {D(CU, kCdm, L"SubscribedContent-338388Enabled", 0), D(CU, kCdm, L"SubscribedContent-338389Enabled", 0),
          D(CU, kCdm, L"SubscribedContent-338393Enabled", 0), D(CU, kCdm, L"SubscribedContent-353694Enabled", 0),
          D(CU, kCdm, L"SubscribedContent-353696Enabled", 0), D(CU, kCdm, L"SystemPaneSuggestionsEnabled", 0),
          D(CU, kCdm, L"SoftLandingEnabled", 0), D(CU, kAdv, L"Start_IrisRecommendations", 0)}},
        {L"silent_apps",
         kGroups[2],
         L"Самоустановка предлагаемых приложений",
         L"Windows не ставит сама «рекомендуемые» игры и программы из Магазина.",
         R,
         {D(CU, kCdm, L"SilentInstalledAppsEnabled", 0), D(CU, kCdm, L"PreInstalledAppsEnabled", 0),
          D(CU, kCdm, L"OemPreInstalledAppsEnabled", 0)}},
        {L"lock_tips",
         kGroups[2],
         L"Советы и реклама на экране блокировки",
         L"На экране блокировки - только картинка, без «Знаете ли вы» и предложений.",
         R,
         {D(CU, kCdm, L"RotatingLockScreenOverlayEnabled", 0), D(CU, kCdm, L"SubscribedContent-338387Enabled", 0)}},
        {L"welcome",
         kGroups[2],
         L"«Добро пожаловать» после обновлений",
         L"После обновлений не открывается рассказ о новинках и «Завершите настройку устройства».",
         R,
         {D(CU, kCdm, L"SubscribedContent-310093Enabled", 0),
          D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\UserProfileEngagement", L"ScoobeSystemSettingEnabled",
            0)}},
        {L"explorer_ads",
         kGroups[2],
         L"Предложения OneDrive в Проводнике",
         L"Проводник не показывает рекламу облачного хранилища.",
         R,
         {D(CU, kAdv, L"ShowSyncProviderNotifications", 0)}},
        {L"account_nag",
         kGroups[2],
         L"Напоминания учётной записи в Пуске",
         L"Пуск не зовёт «завершить настройку учётной записи» и «сделать резервную копию».",
         R,
         {D(CU, kAdv, L"Start_AccountNotifications", 0)}},
        {L"web_search",
         kGroups[2],
         L"Поиск в интернете из Пуска",
         L"Набранное в Пуске не уходит в Bing - поиск только по компьютеру.",
         R,
         {D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", 0),
          D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"CortanaConsent", 0)}},
        {L"web_search_all",
         kGroups[2],
         L"Поиск в интернете для всех",
         L"То же политикой для всех пользователей: поиск Windows не ходит в интернет.",
         R,
         {D(LM, kWSearch, L"DisableWebSearch", 1), D(LM, kWSearch, L"ConnectedSearchUseWeb", 0)}},
        {L"search_highlights",
         kGroups[2],
         L"«Интересное» в поиске",
         L"Поле поиска без картинок дня и подборок новостей.",
         R,
         {D(CU, kSearch, L"IsDynamicSearchBoxEnabled", 0)}},
        {L"search_history",
         kGroups[2],
         L"История поиска на устройстве",
         L"Поиск Windows не запоминает, что вы искали.",
         R,
         {D(CU, kSearch, L"IsDeviceSearchHistoryEnabled", 0)}},
        {L"cloud_search",
         kGroups[2],
         L"Облачный поиск",
         L"Поиск Windows не ищет в OneDrive, Outlook и других облачных сервисах учётной записи.",
         L,
         {D(CU, kSearch, L"IsMSACloudSearchEnabled", 0), D(CU, kSearch, L"IsAADCloudSearchEnabled", 0)}},
        {L"cortana",
         kGroups[2],
         L"Кортана",
         L"Кортана выключена политикой, в том числе на экране блокировки.",
         R,
         {D(LM, kWSearch, L"AllowCortana", 0), D(LM, kWSearch, L"AllowCortanaAboveLock", 0)}},
        {L"copilot",
         kGroups[2],
         L"Кнопка Copilot на панели задач",
         L"Убирает кнопку; сам Copilot остаётся в Пуске.",
         R,
         {D(CU, kAdv, L"ShowCopilotButton", 0)}},
        {L"copilot_off",
         kGroups[2],
         L"Copilot в Windows",
         L"Встроенный Copilot выключен политикой. Приложение Copilot из Магазина это не удаляет.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", 1)}},
        {L"recall",
         kGroups[2],
         L"Recall: снимки экрана для ИИ",
         L"Windows не сохраняет снимки экрана для поиска «по памяти».",
         R,
         {D(LM, kAi, L"DisableAIDataAnalysis", 1), D(LM, kAi, L"TurnOffSavingSnapshots", 1)}},
        {L"click_to_do",
         kGroups[2],
         L"Click to Do",
         L"ИИ-действия над тем, что на экране (Win + щелчок), выключены.",
         R,
         {D(LM, kAi, L"DisableClickToDo", 1)}},
        {L"widgets",
         kGroups[2],
         L"Виджеты и новости",
         L"Панель виджетов с лентой новостей выключена для всех пользователей.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", 0)}},
        {L"pen_suggest",
         kGroups[2],
         L"Предложения приложений для пера",
         L"В меню пера не появляются рекомендации программ из Магазина.",
         R,
         {D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\PenWorkspace", L"PenWorkspaceAppSuggestionsEnabled",
            0)}},

        // ---------------- разрешения приложений ----------------
        {L"app_account",
         kGroups[3],
         L"Сведения учётной записи",
         L"Приложения из Магазина не видят имя, фото и почту учётной записи.",
         R,
         {CONSENT(L"userAccountInformation")}},
        {L"app_contacts",
         kGroups[3],
         L"Контакты",
         L"Приложения из Магазина не читают контакты.",
         L,
         {CONSENT(L"contacts")}},
        {L"app_calendar",
         kGroups[3],
         L"Календарь",
         L"Приложения из Магазина не читают календарь.",
         L,
         {CONSENT(L"appointments")}},
        {L"app_calls",
         kGroups[3],
         L"Журнал звонков",
         L"Приложения из Магазина не видят историю звонков.",
         L,
         {CONSENT(L"phoneCallHistory")}},
        {L"app_email", kGroups[3], L"Почта", L"Приложения из Магазина не читают почту.", L, {CONSENT(L"email")}},
        {L"app_messages",
         kGroups[3],
         L"Сообщения",
         L"Приложения из Магазина не читают SMS и MMS.",
         L,
         {CONSENT(L"chat")}},
        {L"app_tasks",
         kGroups[3],
         L"Задачи",
         L"Приложения из Магазина не видят списки задач.",
         L,
         {CONSENT(L"userDataTasks")}},
        {L"app_radios",
         kGroups[3],
         L"Радиомодули",
         L"Приложения из Магазина не включают и не выключают Bluetooth и Wi-Fi.",
         L,
         {CONSENT(L"radios")}},
        {L"app_location",
         kGroups[3],
         L"Местоположение для приложений",
         L"Приложения из Магазина не узнают, где вы (только для вас, без прав).",
         L,
         {CONSENT(L"location")}},
        {L"app_webcam",
         kGroups[3],
         L"Камера для приложений из Магазина",
         L"Камера закрыта для приложений из Магазина - Камера Windows, Teams из Магазина и подобные не увидят её.",
         N,
         {CONSENT(L"webcam")}},
        {L"app_mic",
         kGroups[3],
         L"Микрофон для приложений из Магазина",
         L"Микрофон закрыт для приложений из Магазина - голосовой ввод и звонки в них перестанут работать.",
         N,
         {CONSENT(L"microphone")}},

        // ---------------- синхронизация и облако ----------------
        {L"settings_sync",
         kGroups[4],
         L"Синхронизация настроек",
         L"Тема, пароли Wi-Fi и параметры не уходят в облако учётной записи и не приходят с других ПК.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSync", 2),
          D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSyncUserOverride", 1)}},
        {L"cloud_clip",
         kGroups[4],
         L"Облачный буфер обмена",
         L"Скопированное не уходит на другие устройства. Журнал буфера (Win+V) на этом ПК работает.",
         L,
         {D(LM, kSys, L"AllowCrossDeviceClipboard", 0)}},
        {L"msg_sync",
         kGroups[4],
         L"Резервная копия сообщений",
         L"SMS с телефона не сохраняются в облаке.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Messaging", L"AllowMessageSync", 0)}},
        {L"onedrive",
         kGroups[4],
         L"OneDrive",
         L"OneDrive перестаёт синхронизировать файлы. Если вы храните в нём документы - не включайте.",
         N,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\OneDrive", L"DisableFileSyncNGSC", 1)}},

        // ---------------- обновления и защита ----------------
        {L"p2p_updates",
         kGroups[5],
         L"Раздача обновлений другим ПК",
         L"Обновления качаются только с серверов Microsoft, ваш канал не раздаёт их чужим компьютерам.",
         R,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization", L"DODownloadMode", 0)}},
        {L"no_auto_reboot",
         kGroups[5],
         L"Перезагрузка после обновлений, пока вы в системе",
         L"Windows не перезагружается сама, пока кто-то вошёл. Не забывайте перезагружать вручную.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"NoAutoRebootWithLoggedOnUsers", 1)}},
        {L"wu_drivers",
         kGroups[5],
         L"Драйверы через Центр обновления",
         L"Драйверы не обновляются вместе с Windows. Свежие драйверы придётся ставить самим.",
         N,
         {D(LM, kWu, L"ExcludeWUDriversInQualityUpdate", 1)}},
        {L"defender_samples",
         kGroups[5],
         L"Отправка образцов в Defender",
         L"Подозрительные файлы не уходят на проверку в облако - защита от новых угроз слабее. При включённой "
         L"«Защите от подделки» Defender может не принять.",
         N,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Spynet", L"SubmitSamplesConsent", 2),
          D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Spynet", L"SpynetReporting", 0)}},
        {L"remote_help",
         kGroups[5],
         L"Удалённый помощник",
         L"К компьютеру нельзя подключиться приглашением «Удалённого помощника».",
         R,
         {D(LM, L"SYSTEM\\CurrentControlSet\\Control\\Remote Assistance", L"fAllowToGetHelp", 0)}},
        {L"wifi_hotspots",
         kGroups[5],
         L"Подключение к предложенным точкам Wi-Fi",
         L"Ноутбук сам не подключается к открытым точкам доступа из списков Microsoft.",
         R,
         {D(LM, L"SOFTWARE\\Microsoft\\WcmSvc\\wifinetworkmanager\\config", L"AutoConnectAllowedOEM", 0)}},

        // ---------------- Microsoft Edge ----------------
        {L"edge_diag",
         kGroups[6],
         L"Диагностика Edge",
         L"Edge не отправляет данные о работе. В настройках Edge появится «Управляется организацией».",
         R,
         {D(LM, kEdge, L"DiagnosticData", 0)}},
        {L"edge_personal",
         kGroups[6],
         L"Реклама и подборки по истории в Edge",
         L"История просмотров не используется для персональной рекламы и новостей.",
         R,
         {D(LM, kEdge, L"PersonalizationReportingEnabled", 0)}},
        {L"edge_shopping",
         kGroups[6],
         L"Помощник покупок в Edge",
         L"Edge не сравнивает цены и не предлагает купоны на сайтах магазинов.",
         R,
         {D(LM, kEdge, L"EdgeShoppingAssistantEnabled", 0)}},
        {L"edge_suggest",
         kGroups[6],
         L"Подсказки поиска в адресной строке Edge",
         L"Набираемое в адресной строке не уходит поисковику до нажатия Enter.",
         L,
         {D(LM, kEdge, L"SearchSuggestEnabled", 0)}},
        {L"edge_background",
         kGroups[6],
         L"Edge в фоне",
         L"Edge не запускается вместе с Windows и не висит в фоне после закрытия. Первое открытие - чуть дольше.",
         L,
         {D(LM, kEdge, L"StartupBoostEnabled", 0), D(LM, kEdge, L"BackgroundModeEnabled", 0)}},

        // ---------------- разное ----------------
        {L"lock_camera",
         kGroups[7],
         L"Камера на экране блокировки",
         L"С экрана блокировки нельзя включить камеру.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization", L"NoLockScreenCamera", 1)}},
        {L"lock_toasts",
         kGroups[7],
         L"Уведомления на экране блокировки",
         L"Тексты сообщений не видны на заблокированном экране.",
         L,
         {D(CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications", L"LockScreenToastEnabled", 0)}},
        {L"clip_history",
         kGroups[7],
         L"Журнал буфера обмена",
         L"Win+V не хранит скопированное. Удобный журнал пропадёт - включайте, только если копируете пароли.",
         L,
         {D(CU, L"Software\\Microsoft\\Clipboard", L"EnableClipboardHistory", 0)}},
        {L"kms_ticket",
         kGroups[7],
         L"Онлайн-проверка лицензии при входе",
         L"Windows не отправляет данные активации при каждом входе. На активацию не влияет.",
         L,
         {D(LM, L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\CurrentVersion\\Software Protection Platform",
            L"NoGenTicket", 1)}},
        {L"track_progs",
         kGroups[7],
         L"Учёт запускаемых программ",
         L"Windows не ведёт счёт запусков. В Пуске пропадёт список «Часто используемые».",
         L,
         {D(CU, kAdv, L"Start_TrackProgs", 0)}},
    };
    return all;
}

#undef CONSENT

const Setting* Find(const std::wstring& id) {
    for (const Setting& s : Catalog())
        if (id == s.id) return &s;
    return nullptr;
}

std::vector<std::wstring> Groups() {
    std::vector<std::wstring> out;
    for (const Setting& s : Catalog())
        if (std::find(out.begin(), out.end(), s.group) == out.end()) out.push_back(s.group);
    return out;
}

// ---------------- бэкап ----------------

namespace {

std::wstring Escape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\')
            o += L"\\\\";
        else if (c == L'\t')
            o += L"\\t";
        else if (c == L'\n')
            o += L"\\n";
        else if (c == L'\r')
            o += L"\\r";
        else
            o += c;
    }
    return o;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 == s.size()) {
            o += s[i];
            continue;
        }
        wchar_t n = s[++i];
        o += n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
    }
    return o;
}

REGSAM View(HKEY root) { return root == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0; }

bool SaveEntry(HKEY root, const std::wstring& key, const Saved& s) {
    HKEY k;
    if (RegCreateKeyExW(root, kBackup, 0, nullptr, 0, KEY_SET_VALUE | View(root), nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    std::wstring data = EncodeSaved(s);
    bool ok = RegSetValueExW(k, key.c_str(), 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * 2)) ==
              ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool DropEntry(HKEY root, const std::wstring& key) {
    HKEY k;
    if (RegOpenKeyExW(root, kBackup, 0, KEY_SET_VALUE | View(root), &k) != ERROR_SUCCESS) return false;
    LONG rc = RegDeleteValueW(k, key.c_str());
    RegCloseKey(k);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

}  // namespace

std::wstring BackupKey(const RegValue& v) { return std::wstring(v.key) + L"|" + v.name; }

std::wstring EncodeSaved(const Saved& s) {
    return std::wstring(s.had ? L"1" : L"0") + L"\t" + std::to_wstring(s.type) + L"\t" + Escape(s.value);
}

bool DecodeSaved(const std::wstring& text, Saved& out) {
    size_t a = text.find(L'\t'), b = a == std::wstring::npos ? a : text.find(L'\t', a + 1);
    if (b == std::wstring::npos || (text.compare(0, a, L"1") != 0 && text.compare(0, a, L"0") != 0)) return false;
    out.had = text[0] == L'1';
    out.type = (DWORD)wcstoul(text.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
    out.value = Unescape(text.substr(b + 1));
    return true;
}

Backup LoadBackup(HKEY root) {
    Backup out;
    HKEY k;
    if (RegOpenKeyExW(root, kBackup, 0, KEY_QUERY_VALUE | View(root), &k) != ERROR_SUCCESS) return out;
    std::vector<wchar_t> name(16384), data(4096);
    for (DWORD i = 0;; ++i) {
        DWORD nl = (DWORD)name.size(), dl = (DWORD)(data.size() * 2), type = 0;
        LONG rc = RegEnumValueW(k, i, name.data(), &nl, nullptr, &type, (BYTE*)data.data(), &dl);
        if (rc == ERROR_MORE_DATA) {  // длинное значение: расширить и прочитать это
                                      // же ещё раз
            data.resize(dl / 2 + 2);
            --i;
            continue;
        }
        if (rc != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        std::wstring text(data.data(), dl / 2);
        while (!text.empty() && text.back() == 0) text.pop_back();
        Saved s;
        if (DecodeSaved(text, s)) out[std::wstring(name.data(), nl)] = s;
    }
    RegCloseKey(k);
    return out;
}

// ---------------- реестр ----------------

bool IsAdmin() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

namespace {

// текущее значение как в бэкапе: тип и строка (число - десятичной, двоичное -
// шестнадцатеричной)
bool ReadRaw(const RegValue& v, Saved& out) {
    HKEY k;
    if (RegOpenKeyExW(v.root, v.key, 0, KEY_QUERY_VALUE | View(v.root), &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = RegQueryValueExW(k, v.name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    std::vector<BYTE> data(size + 2, 0);
    ok = ok && RegQueryValueExW(k, v.name, nullptr, &type, data.data(), &size) == ERROR_SUCCESS;
    RegCloseKey(k);
    if (!ok) return false;
    out.had = true;
    out.type = type;
    if (type == REG_DWORD && size >= 4) {
        DWORD n;
        memcpy(&n, data.data(), sizeof(n));
        out.value = std::to_wstring(n);
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        out.value.assign((const wchar_t*)data.data(), size / sizeof(wchar_t));
        while (!out.value.empty() && out.value.back() == 0) out.value.pop_back();
    } else {
        static const wchar_t hex[] = L"0123456789abcdef";
        out.value.clear();
        for (DWORD i = 0; i < size; ++i) out.value += {hex[data[i] >> 4], hex[data[i] & 15]};
    }
    return true;
}

bool Matches(const RegValue& v, const Saved& cur) {
    if (!cur.had) return false;
    if (v.type == REG_DWORD) return cur.type == REG_DWORD && cur.value == std::to_wstring(v.num);
    return (cur.type == REG_SZ || cur.type == REG_EXPAND_SZ) && _wcsicmp(cur.value.c_str(), v.text) == 0;
}

LONG Write(HKEY k, const wchar_t* name, DWORD type, const std::wstring& value) {
    if (type == REG_DWORD) {
        DWORD n = (DWORD)wcstoul(value.c_str(), nullptr, 10);
        return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&n, sizeof(n));
    }
    if (type == REG_SZ || type == REG_EXPAND_SZ)
        return RegSetValueExW(k, name, 0, type, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * 2));
    std::vector<BYTE> bytes;
    for (size_t i = 0; i + 1 < value.size(); i += 2)
        bytes.push_back((BYTE)wcstoul(value.substr(i, 2).c_str(), nullptr, 16));
    return RegSetValueExW(k, name, 0, type, bytes.data(), (DWORD)bytes.size());
}

}  // namespace

Status StatusOf(const Setting& s, const Backup& user, const Backup& machine) {
    Status st;
    size_t on = 0;
    for (const RegValue& v : s.values) {
        Saved cur;
        if (ReadRaw(v, cur) && Matches(v, cur)) ++on;
        const Backup& b = v.root == HKEY_LOCAL_MACHINE ? machine : user;
        st.ours = st.ours || b.count(BackupKey(v));
    }
    st.state = on == 0 ? State::Off : on == s.values.size() ? State::On : State::Partial;
    return st;
}

bool Apply(const Setting& s, bool priv, const Log& log) {
    if (s.Machine() && !IsAdmin()) {
        if (log) log(std::wstring(s.title) + L": нужны права администратора.");
        return false;
    }
    Backup user = LoadBackup(HKEY_CURRENT_USER), machine = LoadBackup(HKEY_LOCAL_MACHINE);
    bool ok = true;
    for (const RegValue& v : s.values) {
        Backup& b = v.root == HKEY_LOCAL_MACHINE ? machine : user;
        std::wstring key = BackupKey(v);
        if (priv) {
            // прежнее значение - в бэкап до первой правки; не записалось - не правим,
            // иначе вернуть будет нечего
            if (!b.count(key)) {
                Saved old;
                if (!ReadRaw(v, old)) old = Saved{};
                if (!SaveEntry(v.root, key, old)) {
                    ok = false;
                    continue;
                }
                b[key] = old;
            }
            HKEY k;
            if (RegCreateKeyExW(v.root, v.key, 0, nullptr, 0, KEY_SET_VALUE | View(v.root), nullptr, &k, nullptr) !=
                ERROR_SUCCESS) {
                ok = false;
                continue;
            }
            std::wstring val = v.type == REG_DWORD ? std::to_wstring(v.num) : std::wstring(v.text);
            ok = Write(k, v.name, v.type, val) == ERROR_SUCCESS && ok;
            RegCloseKey(k);
        } else {
            auto it = b.find(key);
            if (it == b.end()) continue;  // не мы меняли - не трогаем
            HKEY k;
            LONG rc = RegOpenKeyExW(v.root, v.key, 0, KEY_SET_VALUE | View(v.root), &k);
            if (rc == ERROR_SUCCESS) {
                rc = it->second.had ? Write(k, v.name, it->second.type, it->second.value) : RegDeleteValueW(k, v.name);
                RegCloseKey(k);
            }
            // ключа уже нет и значения не было - вернуть и так получилось
            if (rc == ERROR_SUCCESS || (rc == ERROR_FILE_NOT_FOUND && !it->second.had))
                ok = DropEntry(v.root, key) && ok;
            else
                ok = false;  // запись в бэкапе остаётся: вернуть можно будет позже
        }
    }
    if (log) log(std::wstring(s.title) + (ok ? (priv ? L": включено." : L": как было.") : L": не всё получилось."));
    return ok;
}
// ---------------- профиль ----------------

std::wstring EncodeProfile(const std::vector<std::wstring>& ids) {
    std::wstring t = L"# RadialShutUp: включённые настройки\n";
    for (const std::wstring& id : ids) t += id + L"\n";
    return t;
}

std::vector<std::wstring> DecodeProfile(const std::wstring& text) {
    std::vector<std::wstring> out;
    for (const std::wstring& line : Split(text, L'\n')) {
        if (line.empty() || line[0] == L'#' || !Find(line)) continue;
        if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
    }
    return out;
}

// ---------------- точка восстановления ----------------

namespace {
#pragma pack(push, 1)
struct RestorePointInfo {  // RESTOREPOINTINFOW из srrestoreptapi.h
    DWORD dwEventType;
    DWORD dwRestorePtType;
    INT64 llSequenceNumber;
    WCHAR szDescription[256];
};
struct StateMgrStatus {  // STATEMGRSTATUS
    DWORD nStatus;
    INT64 llSequenceNumber;
};
#pragma pack(pop)
}  // namespace

bool CreateRestorePoint(const std::wstring& name, std::wstring& why) {
    if (!IsAdmin()) {
        why = L"нужны права администратора";
        return false;
    }
    HMODULE sr = LoadLibraryW(L"srclient.dll");
    if (!sr) {
        why = L"восстановление системы недоступно";
        return false;
    }
    using Fn = BOOL(WINAPI*)(RestorePointInfo*, StateMgrStatus*);
    auto set = (Fn)(void*)GetProcAddress(sr, "SRSetRestorePointW");
    bool ok = false;
    if (set) {
        RestorePointInfo in{};
        in.dwEventType = 100;     // BEGIN_SYSTEM_CHANGE
        in.dwRestorePtType = 12;  // MODIFY_SETTINGS
        wcsncpy_s(in.szDescription, name.c_str(), _TRUNCATE);
        StateMgrStatus st{};
        if (set(&in, &st)) {
            in.dwEventType = 101;  // END_SYSTEM_CHANGE
            in.llSequenceNumber = st.llSequenceNumber;
            set(&in, &st);
            ok = true;
        } else {
            why = st.nStatus == ERROR_SERVICE_DISABLED ? L"защита системы выключена для диска C:"
                                                       : L"Windows отказала (код " + std::to_wstring(st.nStatus) + L")";
        }
    } else {
        why = L"восстановление системы недоступно";
    }
    FreeLibrary(sr);
    return ok;
}

}  // namespace shutup
