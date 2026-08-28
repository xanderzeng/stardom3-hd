#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace {

bool HasFontProperties(const char* requested, const wchar_t* expected_face,
                       LONG requested_height, LONG expected_height,
                       LONG expected_weight = -1) {
    LOGFONTA request{};
    request.lfHeight = requested_height;
    request.lfCharSet = DEFAULT_CHARSET;
    strcpy_s(request.lfFaceName, requested);
    HFONT font = CreateFontIndirectA(&request);
    if (!font) {
        return false;
    }
    LOGFONTW actual{};
    const bool matches = GetObjectW(font, sizeof(actual), &actual) != 0 &&
        _wcsicmp(actual.lfFaceName, expected_face) == 0 &&
        actual.lfHeight == expected_height &&
        (expected_weight < 0 || actual.lfWeight == expected_weight);
    DeleteObject(font);
    return matches;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        std::fwprintf(
            stderr,
            L"usage: font_replacement_tests <d3d9.dll> <font-name> "
            L"<font-scale> <expected-height>\n");
        return 2;
    }
    const LONG expected_height = std::wcstol(argv[4], nullptr, 10);

    wchar_t temporary_root[MAX_PATH]{};
    wchar_t staged_directory[MAX_PATH]{};
    wchar_t staged_dll[MAX_PATH]{};
    wchar_t staged_ini[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary_root)) {
        return 3;
    }
    swprintf_s(staged_directory, L"%lsStardom3FontTest-%lu",
               temporary_root, GetCurrentProcessId());
    if (!CreateDirectoryW(staged_directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return 4;
    }
    swprintf_s(staged_dll, L"%ls\\d3d9.dll", staged_directory);
    swprintf_s(staged_ini, L"%ls\\Stardom3.Widescreen.ini",
               staged_directory);
    if (!CopyFileW(argv[1], staged_dll, FALSE) ||
        !WritePrivateProfileStringW(L"Widescreen", L"FontName", argv[2],
                                    staged_ini) ||
        !WritePrivateProfileStringW(L"Widescreen", L"FontScale", argv[3],
                                    staged_ini) ||
        !WritePrivateProfileStringW(L"Widescreen", L"SmallFontName",
                                    L"SimSun", staged_ini) ||
        !WritePrivateProfileStringW(L"Widescreen", L"SmallFontMaxHeight",
                                    L"16", staged_ini) ||
        !WritePrivateProfileStringW(L"Widescreen", L"SmallFontScale",
                                    L"1.00", staged_ini) ||
        !WritePrivateProfileStringW(L"Widescreen", L"SmallFontWeight",
                                    L"600", staged_ini)) {
        return 5;
    }

    HMODULE proxy = LoadLibraryW(staged_dll);
    if (!proxy) {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 6;
    }

    const bool replaced = HasFontProperties(
        "MingLiU", argv[2], 20, expected_height);
    const bool small_replaced = HasFontProperties(
        "MingLiU", L"SimSun", 12, 12, 600);
    const bool untouched = HasFontProperties(
        "Courier New", L"Courier New", 20, 20);

    FreeLibrary(proxy);
    DeleteFileW(staged_ini);
    DeleteFileW(staged_dll);
    RemoveDirectoryW(staged_directory);
    if (!replaced) {
        std::fwprintf(stderr, L"MingLiU replacement properties mismatch\n");
    }
    if (!untouched) {
        std::fwprintf(stderr, L"a non-MingLiU font was unexpectedly changed\n");
    }
    if (!small_replaced) {
        std::fwprintf(stderr, L"small MingLiU routing mismatch\n");
    }
    return replaced && small_replaced && untouched ? 0 : 7;
}
