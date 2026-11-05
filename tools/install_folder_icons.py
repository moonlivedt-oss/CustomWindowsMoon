"""Windows Explorer folder icons. Run with --restore to undo metadata changes.

Uses relative IconResource paths. No Explorer restart or icon-cache deletion.
"""
import argparse
import configparser
import ctypes
import json
import os
import shutil
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
BACKUP = ROOT / 'backups' / 'explorer-folder-icons'
MANIFEST = BACKUP / 'original-state.json'
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
shell = ctypes.WinDLL('shell32', use_last_error=True)
kernel.GetFileAttributesW.argtypes = [wintypes.LPCWSTR]
kernel.GetFileAttributesW.restype = wintypes.DWORD
kernel.SetFileAttributesW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
kernel.SetFileAttributesW.restype = wintypes.BOOL
shell.SHChangeNotify.argtypes = [wintypes.LONG, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p]
shell.SHChangeNotify.restype = None


class FolderSettings(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('dwMask', wintypes.DWORD),
                ('pvid', ctypes.c_void_p), ('pszWebViewTemplate', wintypes.LPWSTR),
                ('cchWebViewTemplate', wintypes.DWORD), ('pszWebViewTemplateVersion', wintypes.LPWSTR),
                ('pszInfoTip', wintypes.LPWSTR), ('cchInfoTip', wintypes.DWORD),
                ('pclsid', ctypes.c_void_p), ('dwFlags', wintypes.DWORD),
                ('pszIconFile', wintypes.LPWSTR), ('cchIconFile', wintypes.DWORD),
                ('iIconIndex', ctypes.c_int), ('pszLogo', wintypes.LPWSTR), ('cchLogo', wintypes.DWORD)]


shell.SHGetSetFolderCustomSettings.argtypes = [ctypes.POINTER(FolderSettings), wintypes.LPCWSTR, wintypes.DWORD]
shell.SHGetSetFolderCustomSettings.restype = ctypes.c_long


def attrs(path):
    value = kernel.GetFileAttributesW(str(path))
    if value == 0xFFFFFFFF:
        raise ctypes.WinError(ctypes.get_last_error())
    return value


def set_attrs(path, value):
    if not kernel.SetFileAttributesW(str(path), value):
        raise ctypes.WinError(ctypes.get_last_error())


def notify(folder):
    # SHCNE_UPDATEITEM | SHCNF_PATHW; ask Explorer to reread the folder.
    shell.SHChangeNotify(0x2000, 0x5, ctypes.cast(ctypes.c_wchar_p(str(folder)), ctypes.c_void_p), None)


def read_ini(path):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    if path.exists():
        data = path.read_bytes()
        if data.startswith((b'\xff\xfe', b'\xfe\xff')):
            text = data.decode('utf-16')
        else:
            try:
                text = data.decode('utf-8-sig')
            except UnicodeDecodeError:
                text = data.decode('mbcs')
        config.read_string(text)
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--restore', action='store_true')
    args = parser.parse_args()
    BACKUP.mkdir(parents=True, exist_ok=True)
    state = json.loads(MANIFEST.read_text(encoding='utf-8')) if MANIFEST.exists() else {}
    if args.restore:
        for name, old in state.items():
            folder = WORK / name
            ini = folder / 'desktop.ini'
            if ini.exists():
                set_attrs(ini, 0x80)
            if old['ini_existed']:
                shutil.copy2(BACKUP / name / 'desktop.ini', ini)
                set_attrs(ini, old['ini_attributes'])
            elif ini.exists():
                ini.unlink()
            set_attrs(folder, old['folder_attributes'])
            notify(folder)
        print(f'Restored folder metadata: {len(state)} folders')
        return

    projects = json.loads((ROOT / 'docs/design/identity/manifest.json').read_text(encoding='utf-8'))['projects']
    for name in projects:
        folder = WORK / name
        ini = folder / 'desktop.ini'
        if name not in state:
            state[name] = {'folder_attributes': attrs(folder), 'ini_existed': ini.exists(),
                           'ini_attributes': attrs(ini) if ini.exists() else None}
            if ini.exists():
                saved = BACKUP / name / 'desktop.ini'
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ini, saved)
            # Persist rollback information before touching each folder.
            MANIFEST.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
        source = ROOT / 'docs/design/identity/logos' / f'{name}.ico'
        if name == 'image':
            # Image library files must be managed through its API.
            # Only Explorer metadata is written here; artwork stays outside it.
            resource = os.path.relpath(source, folder)
        else:
            target = folder / 'branding/folder.ico'
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            resource = r'branding\folder.ico'
        config = read_ini(ini)
        if not config.has_section('.ShellClassInfo'):
            config.add_section('.ShellClassInfo')
        section = config['.ShellClassInfo']
        for key in list(section):
            if key.lower() in ('iconfile', 'iconindex', 'iconresource'):
                del section[key]
        section['IconResource'] = resource + ',0'
        if ini.exists():
            set_attrs(ini, 0x80)
        with ini.open('w', encoding='utf-16', newline='\r\n') as output:
            config.write(output, space_around_delimiters=False)
        set_attrs(ini, 0x2 | 0x4)  # Hidden + System
        set_attrs(folder, attrs(folder) | 0x1)  # ReadOnly tells Shell to read desktop.ini
        settings = FolderSettings()
        settings.dwSize = ctypes.sizeof(settings)
        settings.dwMask = 0x10  # FCSM_ICONFILE
        settings.pszIconFile = resource
        settings.iIconIndex = 0
        result = shell.SHGetSetFolderCustomSettings(ctypes.byref(settings), str(folder), 0x2)
        if result < 0:
            raise OSError(f'SHGetSetFolderCustomSettings failed: {result:#x}: {folder}')
        notify(folder)
    # SHCNE_ASSOCCHANGED: invalidate Shell's cached icon mapping, no processes killed.
    shell.SHChangeNotify(0x08000000, 0, None, None)
    print(f'Installed Explorer icons: {len(projects)} folders. Originals: {BACKUP}')


if __name__ == '__main__':
    main()
