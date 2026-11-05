#pragma once
namespace ui::workspace {
enum Command {
    AddFolder = 1, Rescan, AutoScan, Normalize, Remaining, MiniTop, ReduceMotion,
    Compact, Mini, OpenFiles, OpenFolder, ImportTheme, ExportTheme, PluginsFolder,
    EnableDownloader, SubmitDownload, FontSmaller, FontLarger, Exit, Stats, OpenLibrary, OpenDownloads,
    PreviewMini = 40, PreviewFull, PreviewLarge, UndoSettings,
    SettingsAppearance, SettingsSound, SettingsLibrary, SettingsGeneral, LikedMusic, HideDuplicates,
    BrowserNone, BrowserFirefox, BrowserChrome, BrowserEdge,
    ShapeFirst = 54, MotionFirst = 57, AccentFirst = 63, AccentFromCover = 69,
    AmbientFirst = 70, Visualizer = 73, VizLess, VizMore, CrossfadeFirst = 76,  // 76..79: стык, 3, 6, 10 с
    WindowFxFirst = 80,  // 80..86: появление окон (авто, пять эффектов, без эффекта)
    CustomAccent = 87, ResetSection, UndoAll, IntroToggle = 259, HighContrastToggle = 258, FollowSystemMotion = 257,
    SettingsTabFirst = 90,  // 90..94: Вид, Движение, Звук, Библиотека и загрузки, Общее
    // Ползунки: значение приходит долей 0..1 через App::SliderCommand
    SliderTextScale = 95, SliderVizBars, SliderCrossfade, SliderSpeed, SliderAmbient,
    ShortcutFirst = 260,  // 260..279: переназначить клавишу действия
    PresetFirst = 280,    // 280..283: готовые наборы
    // 284..291: «Волна» под настроение: утро, день, вечер, ночь, забытое, любимое, новое, исполнитель
    WaveMoodFirst = 284,
    CoverFrameFirst = 240,  // 240..250: рамка обложки - нет и десять рамок
    ThemeFirst = 100, RemoveFolderFirst = 300, PluginFirst = 600, PluginMenuFirst = 1000,
    PageActionFirst = 2000
};
}
