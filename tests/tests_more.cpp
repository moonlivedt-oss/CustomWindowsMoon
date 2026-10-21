// Тесты ревью 2026-10-21: radial.cfg туда и обратно (экранирование «|», служебные имена групп) и новые правила
// без окон.
#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/edgecheck.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

void TestConfigRoundTrip() {
    Config c;
    Item g;
    g.name = L"Settings";  // группа с именем служебного раздела
    g.path = L"C:\\";
    Item ch;
    ch.name = L"a|b";
    ch.path = L"macro:text x|y; enter";
    ch.icon = L"code.png";
    g.children.push_back(ch);
    Item at;
    at.name = L"@left";
    c.left = {g, at};
    Item r;
    r.name = L"Проекты";
    r.path = L"D:\\Desktop\\components";
    Item rc;
    rc.name = L"Папка\nс переводом";
    rc.path = L"D:\\Desktop\\";
    r.children.push_back(rc);
    c.right = {r};
    std::wstring file = TempFile(L"radial_test_roundtrip.cfg");
    CHECK(SaveConfig(file, c));
    Config back;
    CHECK(LoadConfig(file, back));
    CHECK(back.left.size() == 2 && back.right.size() == 1);
    if (back.left.size() == 2) {
        CHECK(back.left[0].name == L"Settings");
        CHECK(back.left[0].children.size() == 1);
        if (!back.left[0].children.empty()) {
            CHECK(back.left[0].children[0].name == L"a|b");
            CHECK(back.left[0].children[0].path == L"macro:text x|y; enter");
            CHECK(back.left[0].children[0].icon == L"code.png");
        }
        CHECK(back.left[1].name == L"@left");
    }
    if (back.right.size() == 1 && back.right[0].children.size() == 1) {
        CHECK(back.right[0].children[0].name == L"Папкас переводом");
        CHECK(back.right[0].children[0].path == L"D:\\Desktop\\");  // «\» в конце пути не съедает разделитель
    }
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".tmp").c_str());
}

void TestEdgeCheck() {
    std::string on = "{\"startup\":true,\"enabled\":{\"AlwaysOnTop\":false,\"CursorWrap\" : true,\"FancyZones\":true}}";
    std::string off = "{\"enabled\":{\"CursorWrap\":false},\"x\":{\"CursorWrap\":true}}";
    CHECK(edgecheck::PowerToysEnabled(on, "CursorWrap"));
    CHECK(edgecheck::PowerToysEnabled(on, "FancyZones"));
    CHECK(!edgecheck::PowerToysEnabled(on, "AlwaysOnTop"));
    CHECK(!edgecheck::PowerToysEnabled(off, "CursorWrap"));  // включён только вне "enabled" - не считается
    CHECK(!edgecheck::PowerToysEnabled("", "CursorWrap"));
}

}  // namespace

void TestMore(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestConfigRoundTrip();
    TestEdgeCheck();
}
