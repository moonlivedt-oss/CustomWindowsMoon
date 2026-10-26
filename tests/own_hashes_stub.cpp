// Тесты без своих exe: вшитых хэшей нет (в RadialMenu.exe их пишет cmake/ownhash.cmake).
namespace tools {

const wchar_t* OwnHash(const wchar_t*) { return L""; }

}  // namespace tools
