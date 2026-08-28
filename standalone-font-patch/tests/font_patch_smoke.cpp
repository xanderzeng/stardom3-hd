#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cwchar>

namespace {

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

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
    if (argc != 7) {
        std::fwprintf(
            stderr,
            L"usage: font_patch_smoke <d3d9.dll> <enabled> <font-name> "
            L"<font-scale> <expected-face> <expected-height>\n");
        return 2;
    }
    const LONG expected_height = std::wcstol(argv[6], nullptr, 10);

    wchar_t temp_root[MAX_PATH]{};
    wchar_t staged_directory[MAX_PATH]{};
    wchar_t staged_dll[MAX_PATH]{};
    wchar_t staged_ini[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp_root)) {
        return 3;
    }
    swprintf_s(staged_directory, L"%lsStardom3FontOnly-%lu",
               temp_root, GetCurrentProcessId());
    if (!CreateDirectoryW(staged_directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return 4;
    }
    swprintf_s(staged_dll, L"%ls\\d3d9.dll", staged_directory);
    swprintf_s(staged_ini, L"%ls\\Stardom3.FontPatch.ini",
               staged_directory);
    if (!CopyFileW(argv[1], staged_dll, FALSE) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"Enabled", argv[2], staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"FontName", argv[3], staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"FontScale", argv[4], staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"SmallFontName", L"SimSun", staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"SmallFontMaxHeight", L"16", staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"SmallFontScale", L"1.00", staged_ini) ||
        !WritePrivateProfileStringW(
            L"FontPatch", L"SmallFontWeight", L"600", staged_ini)) {
        return 5;
    }

    HMODULE proxy = LoadLibraryW(staged_dll);
    if (!proxy) {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 6;
    }
    const auto create9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(proxy, "Direct3DCreate9"));
    const bool exports_d3d9 = create9 != nullptr &&
        GetProcAddress(proxy, "Direct3DCreate9Ex") != nullptr;
    IDirect3D9* d3d = create9 ? create9(D3D_SDK_VERSION) : nullptr;
    const bool forwards_d3d9 = d3d != nullptr;
    if (d3d) {
        d3d->Release();
    }
    const bool replaced = HasFontProperties(
        "MingLiU", argv[5], 20, expected_height);
    const bool enabled = std::wcstol(argv[2], nullptr, 10) != 0;
    const bool small_replaced = HasFontProperties(
        "MingLiU", enabled ? L"SimSun" : L"MingLiU", 12, 12,
        enabled ? 600 : -1);
    const bool untouched = HasFontProperties(
        "Courier New", L"Courier New", 20, 20);

    FreeLibrary(proxy);
    const bool restored = HasFontProperties(
        "MingLiU", L"MingLiU", 20, 20);
    DeleteFileW(staged_ini);
    DeleteFileW(staged_dll);
    RemoveDirectoryW(staged_directory);

    if (!exports_d3d9) {
        std::fwprintf(stderr, L"required D3D9 exports are missing\n");
    }
    if (!forwards_d3d9) {
        std::fwprintf(stderr, L"system Direct3DCreate9 was not forwarded\n");
    }
    if (!replaced) {
        std::fwprintf(stderr, L"MingLiU replacement properties mismatch\n");
    }
    if (!untouched) {
        std::fwprintf(stderr, L"a non-MingLiU font was changed\n");
    }
    if (!small_replaced) {
        std::fwprintf(stderr, L"small MingLiU routing mismatch\n");
    }
    if (!restored) {
        std::fwprintf(stderr, L"font import was not restored on unload\n");
    }
    return exports_d3d9 && forwards_d3d9 && replaced && small_replaced &&
        untouched && restored ? 0 : 7;
}
