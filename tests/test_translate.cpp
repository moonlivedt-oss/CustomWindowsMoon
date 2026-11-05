// Распознанный текст и перевод без окон, OCR и сети: helpers/ocrtext_logic и helpers/translate_logic.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "helpers/markup_logic.h"
#include "helpers/ocrtext_logic.h"
#include "helpers/translate_logic.h"

using ocrtext::Line;

namespace {

Line L_(const wchar_t* t, float x, float y, float w, float h = 20) {
    Line l;
    l.text = t;
    l.x = x;
    l.y = y;
    l.w = w;
    l.h = h;
    return l;
}

markup::Pt FakeMeasure(const std::wstring& text, float size, float wrapW, void*) {
    // половина кегля на знак, перенос по ширине
    float one = text.size() * size * 0.5f;
    float lines = std::max(1.0f, std::ceil(one / std::max(1.0f, wrapW)));
    return {std::min(one, wrapW), lines * size * 1.3f};
}

}  // namespace

void TestTranslate(void (*check)(bool, const char*, int)) {
#define CHECK(x) check((x), #x, __LINE__)
    using namespace ocrtext;

    // ---- осмысленность
    CHECK(TextQuality(L"Обычный текст для распознавания") == 1.0);
    CHECK(TextQuality(L"pa60Tana nepeB0A") < 0.5);
    CHECK(TextQuality(L"123 456 -- !!") == 1.0);      // оценивать нечего
    CHECK(TextQuality(L"report.pdf C:/a/b") == 1.0);  // имя файла и путь не мусор
    CHECK(IsGarbageLine(L"xqzt 7hgf kk3l pa60Tana"));
    CHECK(!IsGarbageLine(L"A1B2C3D4E5F6G7"));  // одно слово может быть ключом: не выбрасываем
    CHECK(!IsGarbageLine(L"Сохранить как"));
    CHECK(IsGarbageBlock(L"A1B2C3D4E5F6G7"));  // а под перевод такое не идёт
    CHECK(IsNoise(L"12:45") && IsNoise(L"x") && IsNoise(L"×") && !IsNoise(L"OK"));

    // ---- код
    CHECK(LooksLikeCode(L"int main() {") && LooksLikeCode(L"auto x = Compute(a, b);") &&
          LooksLikeCode(L"cd C:\\Users"));
    CHECK(LooksLikeCode(L"#include <vector>") && LooksLikeCode(L"x->name"));
    CHECK(!LooksLikeCode(L"Проверьте подключение (см. раздел 2)") &&
          !LooksLikeCode(L"The installer could not find, please"));

    // ---- склейка абзацев: перенос внутри абзаца склеивается
    std::vector<Line> prose = {L_(L"The installer could not find the required file and", 10, 10, 420),
                               L_(L"stopped. Please check your connection and try again", 10, 34, 430),
                               L_(L"later today.", 10, 58, 90)};
    CHECK(CleanText(prose) ==
          L"The installer could not find the required file and stopped. Please check your connection and try again "
          L"later today.");
    // перенос по слогам: без дефиса и пробела
    std::vector<Line> hyph = {L_(L"Этот длинный текст проверяет работу перево-", 10, 10, 420),
                              L_(L"дчика и его настройки", 10, 34, 200)};
    CHECK(CleanText(hyph) == L"Этот длинный текст проверяет работу переводчика и его настройки");
    // короткие подписи и меню - строками
    std::vector<Line> menu = {L_(L"Открыть файл", 10, 10, 100), L_(L"Сохранить как", 10, 34, 110),
                              L_(L"настройки", 10, 58, 70)};
    CHECK(CleanText(menu) == L"Открыть файл\r\nСохранить как\r\nнастройки");
    // код - строками, даже широкий
    std::vector<Line> code = {L_(L"auto value = ComputeSomethingLong(first, second, third);", 10, 10, 450),
                              L_(L"return value->name + other_value->name + more;", 10, 34, 440)};
    CHECK(CleanText(code).find(L"\r\n") != std::wstring::npos);
    // новая фраза с заглавной - новая строка: не абзац, а список фактов
    std::vector<Line> facts = {L_(L"Первая строка довольно длинная и доходит до края", 10, 10, 420),
                               L_(L"Вторая строка тоже длинная и доходит до края тоже", 10, 34, 430)};
    CHECK(CleanText(facts).find(L"\r\n") != std::wstring::npos);
    // пункты списка не склеиваются
    std::vector<Line> bullets = {L_(L"- первый пункт довольно длинного списка до края", 10, 10, 420),
                                 L_(L"- второй пункт довольно длинного списка до края", 10, 34, 420)};
    CHECK(CleanText(bullets).find(L"\r\n") != std::wstring::npos);
    CHECK(CleanText({}).empty());
    // соседние столбцы: движок отдал строки вперемешку, абзац всё равно собран целиком
    std::vector<Line> columns = {L_(L"Warning", 600, 8, 70),
                                 L_(L"The last backup could not be completed because the drive", 10, 10, 420),
                                 L_(L"Do not turn off your computer", 600, 34, 240),
                                 L_(L"is almost full and needs some attention", 10, 34, 300)};
    std::wstring columnsText = CleanText(columns);
    CHECK(columnsText.find(L"because the drive is almost full") != std::wstring::npos);
    // форма: подпись и значение остаются рядом, порядок движка не меняется
    std::vector<Line> form = {L_(L"Имя", 10, 10, 40), L_(L"Иван", 200, 10, 40), L_(L"Город", 10, 34, 50), L_(L"Москва", 300, 34, 60), L_(L"Дом", 10, 58, 40)};
    CHECK(CleanText(form) == L"Имя\r\nИван\r\nГород\r\nМосква\r\nДом");

    // таблица: движок отдал по столбцам, собираем по рядам через табуляцию
    std::vector<Line> table = {L_(L"Файл", 10, 10, 40),    L_(L"report.pdf", 10, 36, 70), L_(L"photo.jpg", 10, 62, 66),
                               L_(L"Размер", 160, 10, 50), L_(L"1,2 МБ", 160, 36, 46),    L_(L"3,4 МБ", 160, 62, 46)};
    CHECK(CleanText(table) == L"Файл\tРазмер\r\nreport.pdf\t1,2 МБ\r\nphoto.jpg\t3,4 МБ");
    // два столбца неровно: не таблица, порядок движка не трогаем
    std::vector<Line> notGrid = {L_(L"Заголовок", 10, 10, 90), L_(L"Справа", 300, 10, 50), L_(L"Ещё", 150, 36, 40),
                                 L_(L"Текст", 420, 36, 40)};
    CHECK(CleanText(notGrid).find(L'\t') == std::wstring::npos);

    // ---- сверка движков
    {
        std::vector<Line> ru = {L_(L"Непо", 10, 10, 50), L_(L"Привет", 10, 40, 60)},
                          en = {L_(L"Hello", 10, 10, 50), L_(L"NpnBeT", 10, 40, 60)};
        std::vector<Line> m = MergeEngines(ru, en);
        CHECK(m.size() == 2 && m[0].text == L"Hello");  // латиницу читает английский
        CHECK(m[1].text == L"Привет");                  // точно русское остаётся
        // строку, которую основной не увидел, добавляет английский, сверху вниз
        std::vector<Line> ru2 = {L_(L"Вторая", 10, 40, 60)},
                          en2 = {L_(L"First", 10, 10, 50), L_(L"BTopan", 10, 40, 60)};
        std::vector<Line> m2 = MergeEngines(ru2, en2);
        CHECK(m2.size() == 2 && m2[0].text == L"First" && m2[1].text == L"Вторая");
        // английская строка, где основной ошибся в паре слов: берём английский
        std::vector<Line> ru4 = {L_(L"Free ир some space оп the drive", 10, 10, 300)}, en4 = {L_(L"Free up some space on the drive", 10, 10, 300)};
        CHECK(MergeEngines(ru4, en4)[0].text == L"Free up some space on the drive");
        // вся строка кириллицей, но слова рваные
        std::vector<Line> ru5 = {L_(L"ВасКир Мападег", 10, 10, 150)}, en5 = {L_(L"Backup Manager", 10, 10, 150)};
        CHECK(MergeEngines(ru5, en5)[0].text == L"Backup Manager");
        // русская строка с длинным английским именем файла: не отдаём её английскому
        std::vector<Line> ru6 = {L_(L"Файл report_final_version.docx в папке", 10, 10, 320)}, en6 = {L_(L"OaiiJ1 report_final_version.docx B nanKe", 10, 10, 320)};
        CHECK(MergeEngines(ru6, en6)[0].text == L"Файл report_final_version.docx в папке");
        // английского нет вовсе
        CHECK(MergeEngines(ru, {}).size() == 2 && MergeEngines(ru, {})[0].text == L"Непо");
        // не тот алфавит: у английского цифры вместо букв, основной лучше
        std::vector<Line> ru3 = {L_(L"окно", 10, 10, 50)}, en3 = {L_(L"0KH0", 10, 10, 50)};
        CHECK(MergeEngines(ru3, en3)[0].text == L"окно");
    }

    // ---- блоки
    {
        std::vector<Line> shot = {L_(L"Заголовок окна", 10, 5, 120, 30),
                                  L_(L"Первая строка абзаца тянется", 10, 60, 300),
                                  L_(L"и продолжается здесь.", 10, 84, 200), L_(L"Кнопка", 500, 300, 60)};
        std::vector<Block> blocks = MakeBlocks(shot);
        CHECK(blocks.size() == 3);
        CHECK(blocks[1].text == L"Первая строка абзаца тянется и продолжается здесь." && blocks[1].lines == 2);
        CHECK(blocks[1].x == 10 && blocks[1].y == 60 && blocks[1].h == 44);
        CHECK(blocks[2].text == L"Кнопка");
        CHECK(MakeBlocks(code)[0].code);
        // короткие строки с заглавной - список: перевод не должен слить их в одну фразу
        std::vector<Line> list = {L_(L"Open the settings page", 10, 10, 160), L_(L"Check the free disk space", 10, 34, 190), L_(L"Contact your administrator", 10, 58, 190)};
        CHECK(MakeBlocks(list)[0].text == L"Open the settings page\nCheck the free disk space\nContact your administrator");
        std::vector<Line> centered = {L_(L"длинная строка посередине", 100, 10, 200), L_(L"короткая", 170, 34, 60)};
        CHECK(MakeBlocks(centered)[0].align == 1);
        std::vector<Line> right = {L_(L"длинная строка справа", 100, 10, 200), L_(L"короткая", 240, 34, 60)};
        CHECK(MakeBlocks(right)[0].align == 2);
    }

    // ---- подготовка картинки
    CHECK(UpscaleFactor(400, 200) == 2);
    CHECK(UpscaleFactor(3000, 1000) > 1.3f && UpscaleFactor(3000, 1000) < 1.34f);
    CHECK(UpscaleFactor(3840, 2160) == 1);
    CHECK(UpscaleFactor(0, 0) == 1);
    {
        std::vector<uint32_t> px(100, 0x505050);
        for (int i = 50; i < 100; ++i) px[(size_t)i] = 0x707070;
        GrayContrast(px.data(), 10, 10, 10);
        CHECK((px[0] & 0xFF) == 0 && (px[99] & 0xFF) == 255);  // тусклое растянуто на весь диапазон
        std::vector<uint32_t> flat(16, 0x808080);
        GrayContrast(flat.data(), 4, 4, 4);
        CHECK((flat[0] & 0xFF) > 100 && (flat[0] & 0xFF) < 150);  // одноцветная не портится
    }
    {
        std::vector<uint32_t> px(400, 0x102030);
        for (int y = 8; y < 12; ++y)
            for (int x = 8; x < 12; ++x) px[(size_t)(y * 20 + x)] = 0xFFFFFF;  // текст посередине
        CHECK(EdgeColor(px.data(), 20, 20, 20, 8, 8, 4, 4) == 0x102030);
        CHECK(EdgeColor(px.data(), 20, 20, 20, 500, 500, 4, 4) == 0x1E1E2E);  // вне картинки - запасной цвет
    }

    // ---- обмен с помощником
    {
        using namespace translate;
        const std::wstring tricky = L"строка\nвторая\tтаб\\косая\r";
        CHECK(Escape(tricky).find_first_of(L"\n\t\r") == std::wstring::npos);
        CHECK(Unescape(Escape(tricky)) == tricky);
        CHECK(Unescape(L"конец\\") == L"конец\\");
        HelperLine r = ParseHelperLine(L"READY\ten>ru,de>ru\r\n");
        CHECK(r.kind == HelperLine::Ready && r.text == L"en>ru,de>ru");
        r = ParseHelperLine(L"OK\t7\tперевод\\nстрока");
        CHECK(r.kind == HelperLine::Ok && r.id == L"7" && r.text == L"перевод\nстрока");
        r = ParseHelperLine(L"ERR\t8\tпроблема");
        CHECK(r.kind == HelperLine::Err && r.id == L"8");
        CHECK(ParseHelperLine(L"FAIL\tnoargos").kind == HelperLine::Fail);
        CHECK(ParseHelperLine(L"DONE").kind == HelperLine::Done);
        CHECK(ParseHelperLine(L"шум из библиотеки").kind == HelperLine::Other);
        CHECK(ParseHelperLine(L"OK\tбез текста").kind == HelperLine::Other);

        // ---- направление
        CHECK(SourceOf(L"Hello world") == L"en" && SourceOf(L"Привет, мир") == L"ru");
        CHECK(SourceOf(L"Откройте файл report.pdf в папке") == L"ru");  // большинство
        CHECK(SourceOf(L"Open the file Отчёт now please") == L"en");
        CHECK(TargetFor(L"en") == L"ru" && TargetFor(L"ru") == L"en");
        CHECK(HasPair(L"en>ru,de>ru", L"en", L"ru") && !HasPair(L"en>ru", L"ru", L"en") && !HasPair(L"", L"en", L"ru"));
        CHECK(!HasPair(L"xen>rux", L"en", L"ru"));

        // ---- секреты не уходят наружу, даже внутри фразы
        CHECK(ContainsSecret(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK"));
        CHECK(ContainsSecret(L"вот мой токен ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK он рабочий"));
        CHECK(ContainsSecret(L"Первая строка\n4111 1111 1111 1111\nтретья"));
        CHECK(ContainsSecret(L"password: hunter2345"));
        CHECK(!ContainsSecret(L"Обычный текст без всяких тайн, и слово password тоже"));
        CHECK(!ContainsSecret(L""));

        // ---- ответ Google
        std::wstring out;
        CHECK(ParseGtx(LR"([[["Привет, ","Hello, ",null,null,10],["мир","world",null,null,3]],null,"en"])", out) &&
              out == L"Привет, мир");
        CHECK(ParseGtx(LR"([[["строка\nс \"кавычками\" \u0416","x"]],null,"en"])", out) &&
              out == L"строка\nс \"кавычками\" Ж");
        CHECK(ParseGtx(LR"([[["a]b[c","x",[["y]",1]]],["d","e"]],null,"en"])", out) && out == L"a]b[cd");
        CHECK(!ParseGtx(L"[null,null,\"en\"]", out));
        CHECK(!ParseGtx(L"<html>captcha</html>", out));
        CHECK(!ParseGtx(L"[[[\"обрыв", out));
        CHECK(!ParseGtx(L"", out));

        // ---- длинный текст
        std::wstring longText;
        for (int i = 0; i < 400; ++i) longText += L"Предложение номер раз. ";
        std::vector<std::wstring> chunks = SplitChunks(longText, 1000);
        size_t total = 0;
        for (const std::wstring& c : chunks) {
            CHECK(c.size() <= 1000);
            total += c.size();
        }
        CHECK(chunks.size() > 5 && total > longText.size() * 9 / 10);
        CHECK(SplitChunks(L"коротко").size() == 1);
        CHECK(SplitChunks(std::wstring(3000, L'x'), 1000).size() == 3);  // без пробелов - всё равно влезает
        CHECK(SplitChunks(L"абзац один\nабзац два", 11).size() == 2);

        std::vector<std::wstring> parts;
        CHECK(SplitBatch(L"а\nб\nв", 3, parts) && parts[2] == L"в");
        CHECK(!SplitBatch(L"а\nб", 3, parts));
        CHECK(SplitBatch(L"", 1, parts));
        CHECK(HttpError(429).find(L"подождите") != std::wstring::npos &&
              HttpError(0).find(L"связи") != std::wstring::npos);

        CHECK(SameText(L"GitHub", L"github") && SameText(L"Привет!", L"ПРИВЕТ") && SameText(L"a.b", L"ab"));
        CHECK(!SameText(L"Save", L"Сохранить"));
    }

    // ---- перевод на снимке
    {
        using namespace markup;
        // помещается как есть
        TextFit fit = FitText(L"Короткая", 200, 30, 20, 400, FakeMeasure, nullptr);
        CHECK(fit.size > 16 && fit.w == 200 && fit.h == 30);
        // одна строка длиннее рамки, справа есть место: рамка шире, шрифт тот же
        fit = FitText(L"Довольно длинный перевод на одну строку", 60, 24, 20, 600, FakeMeasure, nullptr);
        CHECK(fit.w > 60 && fit.w <= 600 && fit.size > 16);
        // места нет: шрифт мельче, но не меньше читаемого
        fit = FitText(std::wstring(300, L'я').c_str(), 120, 40, 30, 120, FakeMeasure, nullptr);
        CHECK(fit.size >= 9 && fit.size < 24 && fit.w == 120);
        // совсем не лезет: рамка выше, текст не обрезан
        CHECK(fit.h >= FakeMeasure(std::wstring(300, L'я'), fit.size, 120, nullptr).y - 1);
        // мелкий оригинал: шрифт не взлетает и не падает ниже оригинала
        fit = FitText(L"ok", 40, 10, 9, 40, FakeMeasure, nullptr);
        CHECK(fit.size >= 7 && fit.size <= 10);

        // расширять подпись можно до соседа на той же высоте, но не поверх него
        CHECK(RoomRight({10, 10, 50, 20}, {{200, 15, 40, 20}}, 900) == 196);
        CHECK(RoomRight({10, 10, 50, 20}, {{200, 100, 40, 20}}, 900) == 900);  // сосед ниже не мешает
        CHECK(RoomRight({10, 10, 50, 20}, {{5, 15, 40, 20}}, 900) == 900);     // сосед слева тоже
        CHECK(RoomRight({10, 10, 50, 20}, {{61, 10, 40, 20}}, 900) == 60);     // вплотную: не меньше самой рамки

        // фигура перевода: двигается, ручек нет, один шаг отмены на всю пачку
        Shape t;
        t.tool = Tool::Translation;
        t.pts = {{10, 10}, {110, 40}};
        CHECK(Meaningful(t) && Handles(t).empty());
        CHECK(HitTest(t, {50, 25}, 0));  // сплошная: щёлкается в любом месте
        Doc doc;
        Shape box;
        box.tool = Tool::Box;
        box.pts = {{0, 0}, {50, 50}};
        doc.Add(box);
        std::vector<Shape> many = doc.Shapes();
        many.push_back(t);
        many.push_back(t);
        doc.SetShapes(many);
        CHECK(doc.Shapes().size() == 3);
        CHECK(doc.Undo() && doc.Shapes().size() == 1);
        CHECK(doc.Redo() && doc.Shapes().size() == 3);
    }
#undef CHECK
}
