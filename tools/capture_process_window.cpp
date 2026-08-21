#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

BOOL CALLBACK FindWindowForPid(HWND hwnd, LPARAM value) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto* result = reinterpret_cast<std::pair<DWORD, HWND>*>(value);
    if (pid == result->first && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        result->second = hwnd;
        return FALSE;
    }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    std::pair<DWORD, HWND> found{pid, nullptr};
    EnumWindows(FindWindowForPid, reinterpret_cast<LPARAM>(&found));
    if (!found.second) return 1;
    RECT client{};
    GetClientRect(found.second, &client);
    POINT origin{0, 0};
    ClientToScreen(found.second, &origin);
    const int width = client.right;
    const int height = client.bottom;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = -height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    GetDIBits(memory, bitmap, 0, height, pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    FILE* output = nullptr;
    _wfopen_s(&output, argv[2], L"wb");
    if (!output) return 1;
    std::fwrite(&file, sizeof(file), 1, output);
    std::fwrite(&header, sizeof(header), 1, output);
    std::fwrite(pixels.data(), pixels.size(), 1, output);
    std::fclose(output);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    std::printf("captured %dx%d origin=%d,%d\n", width, height, origin.x, origin.y);
    return 0;
}
