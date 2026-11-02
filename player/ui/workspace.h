#pragma once
namespace ui::workspace {
enum Command {
    AddFolder = 1, Rescan, AutoScan, Normalize, Remaining, MiniTop, ReduceMotion,
    Compact, Mini, OpenFiles, OpenFolder, ImportTheme, ExportTheme, PluginsFolder,
    EnableDownloader, SubmitDownload, FontSmaller, FontLarger, Exit, Stats, OpenLibrary,
    PreviewMini = 40, PreviewFull, PreviewLarge, UndoSettings,
    SettingsAppearance, SettingsSound, SettingsLibrary, SettingsGeneral, LikedMusic, HideDuplicates,
    BrowserNone, BrowserFirefox, BrowserChrome, BrowserEdge,
    ShapeFirst = 54, MotionFirst = 57, AccentFirst = 63, AccentFromCover = 69,
    AmbientFirst = 70, Visualizer = 73, VizLess, VizMore,
    ThemeFirst = 100, RemoveFolderFirst = 300, PluginFirst = 600, PluginMenuFirst = 1000,
    PageActionFirst = 2000
};
}
