# Что добавить в утилиты: список и ход работ

## Ход работ (2026-10-17, продолжать с этого места)

Выбрано: всё из списка ниже, **кроме Г3, Г4, Д1**. Нижний слой написан и проверен (`--sys-diag`), кольцо и
студия ещё не подключены.

**Готово (код есть, собирается, проверено на этом ПК через `RadialMenu.exe --sys-diag` -> `sys_out.txt`):**
- `src/tools/recovery.cpp` - FindBroken / FixBrokenUser / FixBrokenAdminMain (А4), PendingReboot (В1),
  RestartToRecovery (А6), RepairWindowsAdminMain (А1: DISM, потом SFC), ResetWindowsUpdateAdminMain (А3),
  DefenderOfflineAdminMain (А7).
- `src/tools/privacy.cpp` - PrivacyUses (Б2), CheckCerts (Б3, находка FIND_CERT; нашёл корень «MoonLiveDz»),
  FileOrigin (Б4), CheckFirewall (Б5, FIND_FW). Сертификаты и брандмауэр уже идут в ScanSecurity (hijacks).
- `src/tools/sysinfo.cpp` - DeviceProblems (В3), GpuNow через nvidia-smi (В4), RecordBattery / BatteryHistory
  (В5, файл battery.tsv), WindowsUpdates (В2, COM), SystemReport (В6, текст).
- `src/tools/devtools2.cpp` - GitRepos (Г1), VirtualDisks + CompactVhdAdminMain (Г2), AppLeftovers (Д2).
- RadialTools.exe: команды `explorer-reset` (А5) и `recycle-leftover` (Д2); обёртки ResetExplorer,
  RecycleLeftovers в tools/helper.cpp.
- Задания с правами (net/fix.cpp, net.h): JOB_REPAIR, JOB_WU_RESET, JOB_UNBREAK, JOB_DEFENDER_OFFLINE,
  JOB_VHD_COMPACT.
- toolsui.cpp: фоновый RefreshScan считает git, vhd, leftovers, devices, updates (Scan); слежка пишет батарею
  раз в минуту и в игре меряет видеокарту раз в 10 с (g_gpu), GameGpuSummary - карточка после игры;
  в «Очистке» уже показаны пункты «Docker и WSL» (ключ `vhd`) и «Хвост ...» (ключи `left:i`).
- Защита: свой локальный сертификат подписи доверен только для своих exe (OwnBinary в security.cpp).

**Осталось:**
1. toolsui.cpp, Run(key): обработчики `vhd` (Ask: «Почистить Docker» = `docker system prune -f` через
   RunCapture / «Сжать диски» = `wsl --shutdown` + net::RunAdmin(JOB_VHD_COMPACT)) и `left:i`
   (Ask -> tools::RecycleLeftovers).
2. toolsui.cpp: RecoveryItem() - сектор «Восстановление» (значки rec-*.png, починка/точки - tools-repair,
   tools-snapshot): rec-repair (RunAdmin JOB_REPAIR, таймаут 90 мин), sec-restore (уже есть), rec-rstrui
   (rstrui.exe), rec-wu (Ask -> JOB_WU_RESET), rec-unbreak (FindBroken: HKCU - FixBrokenUser, HKLM - JOB_UNBREAK),
   rec-explorer (Ask -> tools::ResetExplorer), rec-boot (Ask -> RestartToRecovery), rec-offline (Kaspersky есть
   -> открыть avpui; иначе Ask -> JOB_DEFENDER_OFFLINE), rec-memo (карточка «если Windows не грузится» +
   RecoveryDrive.exe).
3. toolsui.cpp: SystemItem(scan) - сектор «Система»: ждёт перезагрузки (+ карточка один раз за загрузку в
   TryProactive, кнопка = shutdown /r /t 30), обновления (scan.updates), устройства (scan.devices),
   видеокарта (GpuNow в RunJob), батарея (BatteryHistory: было/стало, совет про лимит заряда в Armoury Crate,
   если mostlyAc), отчёт (RunJob -> SystemReport -> Документы\Отчёт RadialMenu <дата>.txt -> notepad).
4. toolsui.cpp: GitItem(scan) - сектор «Git» (g_git = scan.git, только репо с changed/ahead), `git:i` ->
   OpenEnvTerminal(DetectEnv(path)).
5. «Защита»: пункт «Камера и микрофон» (`sec-privacy`: карточка с последними за 30 дней, дубли по
   программе схлопнуть, «сейчас» первыми, кнопка ms-settings:privacy-webcam); в kWhat добавить
   L"сертификат", L"брандмауэр"; SecurityAction для FIND_CERT (certmgr.msc / certlm.msc) и FIND_FW (wf.msc);
   FileOrigin - в карточку неподписанной программы (sec-app), в карточку Песочницы и в факты SecurityAction.
6. Б1: вкладка «Защита» в студии (studio_tools.cpp, вкладки ToolsPage): находки списком по видам, причина,
   кнопка «Что делать» (экспортировать из toolsui SecurityAction) и «Это моё» (ignored.tsv в DataDir; фильтр
   в SecurityItem, OnSecurity и студии).
7. Б6: подпись своих exe - кнопка «Настроить подпись» (PowerShell New-SelfSignedCertificate CN=MoonRadial
   Local, CodeSigningCert, в CurrentUser\My и CurrentUser\Root - Windows сама спросит подтверждение) и
   POST_BUILD в CMakeLists: Set-AuthenticodeSignature перед cmake/integrity.cmake; без сертификата - пропуск.
7а. Значки набора 3 уже нарезаны в icons/ (26 штук). Подставить: Песочница - tools-sandbox (сейчас tools-shield),
   режим проверки - tools-sandbox-check; «Чисто»/«Тихо» - tools-ok, «Собираю»/«Проверка» - tools-wait (сейчас
   tools-box/tools-shield); находки «Защиты» по виду: FIND_PATH - tools-path-open, FIND_SVC - tools-service,
   FIND_WMI - tools-wmi, FIND_SELF - tools-integrity, FIND_CERT - tools-certs, FIND_FW - tools-firewall.
8. Собрать (`cmake --build build-check`), проверить (`--sys-diag`, `--sec-diag`, `--watch-selftest`),
   build.cmd, запустить, коммит без упоминаний Claude.

**Промпт для новой сессии** (вставить целиком):

> Продолжаем RadialMenu (D:\Desktop\components\RadialMenu). Прочитай NEXT.md, раздел «Ход работ»: нижний слой
> новых утилит уже написан и собирается, осталось подключить его к кольцу «Утилиты» и студии - пункты 1-8 по
> порядку. Код в стиле проекта (комментарии по-русски, без эмодзи), правки строк с обратными слешами - через
> Edit, не через heredoc. Проверочная сборка - build-check, рабочая - build.cmd. Коммить сам, без упоминаний
> Claude; в GitHub не отправляй, пока не скажу.

---

## Список

Составлено 2026-10-17 после ревью. Отметьте `[x]` то, что делаем в следующей сессии.
Не повторяет уже сделанное (диски, очистка, PATH, порты, процессы, фон, автозагрузка, терминал, «Кто держит»,
версии, дубли, «Защита», Песочница, кольцо «Сеть», Boost, игровой режим) и то, от чего вы отказывались
(горячие клавиши, поиск, украшения стола, живое превью кольца).
Значки к пунктам - в `art/ICONS-PROMPTS.md`, раздел «Набор 3».

## А. Свой Recovery Kit: сектор «Восстановление»

То, что делал RecoveryKit, только своё, подписанное сборкой и с понятными шагами. Всё с правами - через
задачу MoonRadial (одно окно UAC), перед правками - точка восстановления.

- [ ] **А1. Починить Windows по шагам**: SFC, при неудаче DISM и снова SFC одним заданием, ход - в карточке
  (сейчас два отдельных окна cmd).
- [ ] **А2. Точки восстановления**: список с датами, «Создать», «Открыть восстановление» (rstrui).
- [ ] **А3. Сбросить Центр обновления**: остановить службы, очистить SoftwareDistribution и catroot2,
  запустить заново - лечит зависшие и сломанные обновления.
- [ ] **А4. Вернуть то, что ломают вирусы**: Диспетчер задач и regedit, если их выключили политикой; запуск
  .exe, .bat, .reg по умолчанию; Shell и Userinit; пустой hosts по умолчанию (с бэкапом). Только то, что
  «Защита» нашла испорченным.
- [ ] **А5. Сбросить Проводник**: кэш значков и эскизов, перезапуск explorer - когда значки «поехали».
- [ ] **А6. Особая загрузка**: перезагрузка в меню восстановления Windows или в безопасный режим одной
  кнопкой (с вопросом и предупреждением).
- [ ] **А7. Офлайн-проверка**: запуск проверки Microsoft Defender Offline или KVRT, если основной антивирус
  не справляется (по вашему выбору, с объяснением, что компьютер перезагрузится).
- [ ] **А8. Аварийная карточка**: что делать, если Windows не грузится (Shift + Перезагрузка, флешка
  восстановления, где точки) - текстом, без действий.

## Б. Безопасность

- [ ] **Б1. Страница «Защита» в студии**: все находки списком с фильтром по виду, причинами и кнопками;
  «Это моё» - больше не показывать (VSCodeCssAutoheal и подобное).
- [ ] **Б2. Кто включал камеру и микрофон**: Windows сама записывает, какая программа и когда брала камеру,
  микрофон, геопозицию (без прав) - список с временем, незнакомое помечается.
- [ ] **Б3. Чужие корневые сертификаты**: сертификаты, которые позволяют подменять сайты (так делают adware и
  «оптимизаторы»); Kaspersky, Docker и подобные - известные, остальные с вопросом.
- [ ] **Б4. Откуда скачан файл**: для неподписанных программ и в Песочнице показывать адрес, с которого файл
  скачан (Windows хранит его в метке файла).
- [ ] **Б5. Брандмауэр**: входящие разрешения для программ из папок пользователя и Загрузок.
- [ ] **Б6. Подписать свои exe** своим сертификатом при сборке: меньше поводов для Kaspersky, помощник с
  правами проверяет подпись.

## В. Система

- [ ] **В1. Windows ждёт перезагрузки**: карточка, когда обновление или компонент ставятся только после
  перезагрузки (как было с Песочницей), и почему «Завершение работы» не помогает.
- [ ] **В2. Обновления Windows**: когда последнее, что ждёт установки, ошибки установки.
- [ ] **В3. Устройства с ошибками**: что в Диспетчере устройств с жёлтым знаком и почему.
- [ ] **В4. Температура и троттлинг**: видеокарта (nvidia-smi) и процессор в игре; после игры - итог,
  упиралась ли она в перегрев.
- [ ] **В5. Износ батареи по неделям**: график ёмкости (сейчас 79,7 из 90 Вт*ч) и совет про ограничение
  заряда в Armoury Crate.
- [ ] **В6. Отчёт о системе одной кнопкой**: железо, диски, драйверы, находки «Защиты» - в один файл, чтобы
  показать, если просите помощи.

## Г. Разработка

- [ ] **Г1. Кольцо Git**: проекты с незакоммиченным и неотправленным (у RadialMenu сейчас 17 коммитов не
  в GitHub), двойной клик - терминал в папке.
- [ ] **Г2. Docker и WSL**: сколько места занимают образы и диски WSL, сжать vhdx и почистить неиспользуемое.
- [ ] **Г3. Сторож сборки Unreal**: карточка, когда шейдеры или упаковка закончились, и сколько ещё ждать.
- [ ] **Г4. Резервная копия проектов**: раз в неделю копия `components` на другой диск без node_modules,
  build и Intermediate.

## Д. Каждый день

- [ ] **Д1. Буфер без секретов**: скопировали токен, ключ или номер карты - через минуту буфер очищается.
- [ ] **Д2. Мусор от удалённых программ**: папки в AppData и ProgramData от того, чего больше нет.
