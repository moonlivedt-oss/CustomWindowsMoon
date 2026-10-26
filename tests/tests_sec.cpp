// Тесты пунктов безопасности (PLAN.md): задачи с правами, ключи телефонов, тормоз неверных ключей и прочее.
#include <windows.h>

#include <string>

#include "core/elevate.h"
#include "phone/phone.h"

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

void TestPhoneHttpsOnly() {
    std::vector<std::string> dev = {"devkey0123456789", "otherkey9876543210"};
    int d = 5;
    CHECK(phone::MatchKey("/tok123/poll", "tok123", dev, &d) == 7 && d == -1);
    CHECK(phone::MatchKey("/tok123", "tok123", dev, &d) == 7);
    CHECK(phone::MatchKey("/tok1234/", "tok123", dev, &d) == 0);  // ключ длиннее - чужой
    CHECK(phone::MatchKey("/d/otherkey9876543210/poll", "tok123", dev, &d) == 21 && d == 1);
    CHECK(phone::MatchKey("/d/otherkey98765432", "tok123", dev, &d) == 0 && d == -1);
    CHECK(phone::MatchKey("/d/", "", {""}, &d) == 0);  // пустой ключ ничего не открывает
    CHECK(phone::MatchKey("/", "", dev, nullptr) == 0);
    CHECK(phone::HttpsLocation("192.168.1.5:8780", 8800, "/d/k/") == "https://192.168.1.5:8800/d/k/");
    CHECK(phone::HttpsLocation("pc.local", 8801, "/d/k/") == "https://pc.local:8801/d/k/");
    CHECK(phone::HttpsLocation("evil.com/x", 8800, "/d/k/").empty());  // Host не дописывает в ответ своё
    CHECK(phone::HttpsLocation("a b", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("[::1]:8780", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("192.168.1.5", 0, "/d/k/").empty());  // https не открыт
    CHECK(phone::HttpsLocation("192.168.1.5", 8800, "d/k/").empty());
    CHECK(phone::HttpsLocation("192.168.1.5", 8800, std::string("/d/k\r\nX: 1")).empty());
}

}  // namespace

void TestSec(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestElevateHelper();
    TestPhoneHttpsOnly();
}
