// Тесты пунктов безопасности (PLAN.md): задачи с правами, ключи телефонов, тормоз неверных ключей и прочее.
#include <windows.h>

#include <string>

#include "core/elevate.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

void TestElevateHelper() {
    std::wstring dir = L"C:\\Program Files\\MoonRadial", hash(64, L'a');
    hash.replace(0, 12, L"0123456789ab");
    CHECK(elevate::HelperName(hash) == L"RadialMenu-0123456789ab.exe");
    CHECK(elevate::InsideDir(dir + L"\\RadialMenu-x.exe", dir));
    CHECK(elevate::InsideDir(L"c:\\program files\\moonradial\\a.exe", dir));  // регистр не важен
    CHECK(!elevate::InsideDir(dir + L"\\data\\a.exe", dir));                  // подпапка (туда пишет задание)
    CHECK(!elevate::InsideDir(dir + L"\\..\\a.exe", dir));
    CHECK(!elevate::InsideDir(dir + L"Evil\\a.exe", dir));  // соседняя папка с тем же началом
    CHECK(!elevate::InsideDir(dir + L"\\a.exe:x", dir));    // поток NTFS
    CHECK(!elevate::InsideDir(dir + L"\\RADIAL~1.EXE", dir));
    CHECK(!elevate::InsideDir(dir, dir));
    CHECK(!elevate::InsideDir(L"D:\\a.exe", L""));
    CHECK(elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ab.exe", dir, hash));
    CHECK(elevate::TrustedHelper(L"C:\\PROGRAM FILES\\MoonRadial\\radialmenu-0123456789AB.exe", dir, hash));
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ac.exe", dir, hash));  // содержимое не то
    CHECK(!elevate::TrustedHelper(L"D:\\Desktop\\RadialMenu\\RadialMenu-0123456789ab.exe", dir, hash));  // не там
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu.exe", dir, hash));
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ab.exe", dir, L""));  // хэш не посчитался
}

}  // namespace

void TestSec(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestElevateHelper();
}
