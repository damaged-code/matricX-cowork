#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <random>
#include <string>

namespace {
constexpr wchar_t className[] = L"MouseWanderBorder";
volatile LONG cursorChanged = 0;

bool restoreSystemCursors() {
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) return false;
    InterlockedExchange(&cursorChanged, 0);
    return true;
}

BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (InterlockedCompareExchange(&cursorChanged, 0, 0)) restoreSystemCursors();
    }
    return FALSE;
}

HCURSOR createAgentCursor() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return nullptr;
    const auto imagePath = std::filesystem::path(executable.data()).parent_path() /
                           L"akar-icons_cursor.png";
    Gdiplus::Bitmap source(imagePath.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok) {
        std::cerr << "Could not load akar-icons_cursor.png beside the executable.\n";
        return nullptr;
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    // Keep more of the PNG's detail and render at the display's physical resolution.
    const int size = std::max(1, MulDiv(32, dpi, 96));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color) return nullptr;
    ZeroMemory(pixels, static_cast<std::size_t>(size) * size * 4);

    bool drawn = false;
    {
        Gdiplus::Bitmap bitmap(size, size, size * 4, PixelFormat32bppPARGB,
                              static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::ImageAttributes attributes;
        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        drawn = graphics.DrawImage(&source, Gdiplus::Rect(0, 0, size, size),
                                  0, 0, source.GetWidth(), source.GetHeight(),
                                  Gdiplus::UnitPixel, &attributes) == Gdiplus::Ok;
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HCURSOR cursor = nullptr;
    if (drawn && mask) {
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc) {
            HGDIOBJ previous = SelectObject(dc, mask);
            if (previous && previous != HGDI_ERROR) {
                PatBlt(dc, 0, 0, size, size, BLACKNESS);
                SelectObject(dc, previous);
                ICONINFO cursorInfo{};
                cursorInfo.xHotspot = cursorInfo.yHotspot =
                    static_cast<DWORD>(std::lround(size * 3.0 / 24.0));
                cursorInfo.hbmColor = color;
                cursorInfo.hbmMask = mask;
                cursor = static_cast<HCURSOR>(CreateIconIndirect(&cursorInfo));
            }
            DeleteDC(dc);
        }
    }
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return cursor;
}

struct AgentCursor {
    // Standard Windows cursor roles: arrow, text, busy, resize, hand, etc.
    const std::array<DWORD, 14> ids{
        32512, 32513, 32514, 32515, 32516, 32642, 32643,
        32644, 32645, 32646, 32648, 32649, 32650, 32651};
    bool active = false;

    ~AgentCursor() { stop(); }

    bool start() {
        HCURSOR custom = createAgentCursor();
        if (!custom) return false;
        if (!SetConsoleCtrlHandler(consoleHandler, TRUE)) {
            DestroyCursor(custom);
            return false;
        }
        active = true;
        InterlockedExchange(&cursorChanged, 1);
        bool success = true;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            HCURSOR replacement = static_cast<HCURSOR>(CopyImage(custom, IMAGE_CURSOR, 0, 0, 0));
            if (!replacement) {
                success = false;
                break;
            }
            if (!SetSystemCursor(replacement, ids[i])) {
                DestroyCursor(replacement);
                success = false;
                break;
            }
        }
        DestroyCursor(custom);
        if (!success) stop();
        return success;
    }

    void stop() {
        if (!active) return;
        // Reload the saved theme; snapshots can already contain the temporary cursor.
        if (!restoreSystemCursors()) {
            std::cerr << "Could not reload Windows cursors; retrying on exit.\n";
            return;
        }
        active = false;
        SetConsoleCtrlHandler(consoleHandler, FALSE);
    }
};

struct GraphicsRuntime {
    ULONG_PTR token = 0;
    bool start() {
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }
    ~GraphicsRuntime() {
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};

LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_MOUSEMOVE) {
        const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if ((event->flags & LLMHF_INJECTED) == 0) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct MouseMovementLock {
    HHOOK hook = nullptr;

    bool start() {
        hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
        return hook != nullptr;
    }

    ~MouseMovementLock() {
        stop();
    }

    void stop() {
        if (hook) UnhookWindowsHookEx(hook);
        hook = nullptr;
    }
};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct Borders {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    bool registered = false;
    bool visible = false;
    BYTE opacity = 0;

    ~Borders() {
        if (window) DestroyWindow(window);
        if (registered) UnregisterClassW(className, instance);
    }

    bool create(int width, int height) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = className;
        if (!RegisterClassW(&wc)) return false;
        registered = true;

        window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            className, L"", WS_POPUP, 0, 0, width, height,
            nullptr, nullptr, instance, nullptr);
        if (!window) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) {
            DeleteDC(dc);
            return false;
        }
        HGDIOBJ previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(dc);
            return false;
        }
        auto* output = static_cast<std::uint32_t*>(pixels);
        const double spread = std::clamp(std::min(width, height) * 0.075, 40.0, 140.0);
        // Combine Gaussian edge glows without seams, keeping the center clear.
        const auto glow = [spread](double distance) {
            const double d = distance / spread;
            return std::exp(-d * d * 2.0);
        };
        for (int y = 0; y < height; ++y) {
            const double vertical = (1.0 - glow(y)) * (1.0 - glow(height - 1 - y));
            for (int x = 0; x < width; ++x) {
                const double fade = 1.0 - vertical * (1.0 - glow(x)) * (1.0 - glow(width - 1 - x));
                const auto alpha = static_cast<std::uint32_t>(std::lround(fade * 0.72 * 255));
                const auto red = static_cast<std::uint32_t>(std::lround(alpha * 0.12));
                const auto green = static_cast<std::uint32_t>(std::lround(alpha * 0.94));
                const auto blue = static_cast<std::uint32_t>(std::lround(alpha * 0.48));
                output[static_cast<std::size_t>(y) * width + x] =
                    (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        POINT origin{};
        SIZE size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};
        const BOOL updated = UpdateLayeredWindow(
            window, nullptr, &origin, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        const DWORD error = updated ? ERROR_SUCCESS : GetLastError();
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (!updated) SetLastError(error);
        return updated != FALSE;
    }

    bool setOpacity(double value) {
        const BYTE next = static_cast<BYTE>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
        if (next == opacity) return true;
        BLENDFUNCTION blend{AC_SRC_OVER, 0, next, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, 0, &blend, ULW_ALPHA)) return false;
        opacity = next;
        return true;
    }

    void show(bool value) {
        if (visible == value) return;
        ShowWindow(window, value ? SW_SHOWNOACTIVATE : SW_HIDE);
        visible = value;
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--restore-cursor") {
        if (!restoreSystemCursors()) {
            std::cerr << "Could not restore Windows cursors.\n";
            return 1;
        }
        std::cout << "Windows cursor theme restored.\n";
        return 0;
    }
    SetProcessDPIAware();
    GraphicsRuntime graphics;
    if (!graphics.start()) {
        std::cerr << "Could not initialize cursor rendering.\n";
        return 1;
    }
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0) {
        std::cerr << "Could not determine screen size.\n";
        return 1;
    }

    Borders borders;
    if (!borders.create(width, height)) {
        std::cerr << "Could not create screen borders: " << GetLastError() << '\n';
        return 1;
    }

    POINT current{};
    if (!GetCursorPos(&current)) {
        std::cerr << "Could not read the cursor position.\n";
        return 1;
    }
    POINT start = current;
    POINT target = current;
    std::mt19937 random(std::random_device{}());
    std::uniform_int_distribution<int> randomX(0, width - 1);
    std::uniform_int_distribution<int> randomY(0, height - 1);
    std::uniform_int_distribution<int> randomSide(0, 3);
    std::uniform_int_distribution<int> randomSteps(15, 39);
    std::bernoulli_distribution chooseEdge(0.45);
    int step = 0, steps = 0, dwell = 0;
    bool edgePending = false;

    const UINT_PTR timer = SetTimer(nullptr, 0, 10, nullptr);
    if (!timer) {
        std::cerr << "Could not create the animation timer.\n";
        return 1;
    }
    MouseMovementLock mouseLock;
    if (!mouseLock.start()) {
        std::cerr << "Could not block manual mouse movement: " << GetLastError() << '\n';
        KillTimer(nullptr, timer);
        return 1;
    }
    AgentCursor cursor;
    if (!cursor.start()) {
        std::cerr << "Could not install the custom cursor.\n";
        KillTimer(nullptr, timer);
        return 1;
    }
    borders.show(true);
    const auto started = std::chrono::steady_clock::now();
    std::cout << "Moving the mouse for 60 seconds. Manual movement is blocked. "
                 "Press Escape to stop.\n";
    bool running = true;
    bool stopping = false;
    auto stopped = started;
    double fadeOutFrom = 1.0;
    int result = 0;
    while (running) {
        MSG message{};
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            if (received == -1) result = 1;
            break;
        }
        if (message.message != WM_TIMER || message.wParam != timer) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!stopping && ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
                         now - started >= std::chrono::seconds(60))) {
            mouseLock.stop();
            cursor.stop();
            stopping = true;
            stopped = now;
            fadeOutFrom = borders.opacity / 255.0;
        }
        if (stopping) {
            const double t = std::clamp(
                std::chrono::duration<double>(now - stopped).count() / 0.22, 0.0, 1.0);
            if (!borders.setOpacity(fadeOutFrom * (1.0 - t * t * (3.0 - 2.0 * t)))) {
                result = 1;
                break;
            }
            if (t >= 1.0) break;
            continue;
        }
        const double tIn = std::clamp(
            std::chrono::duration<double>(now - started).count() / 0.4, 0.0, 1.0);
        if (!borders.setOpacity(1.0 - std::pow(1.0 - tIn, 3))) {
            std::cerr << "Could not update the green glow.\n";
            result = 1;
            break;
        }

        bool move = true;
        if (dwell > 0) {
            --dwell;
            move = false;
        } else if (step >= steps) {
            if (edgePending) {
                edgePending = false;
                dwell = 75;
                move = false;
            } else {
                start = current;
                target = {randomX(random), randomY(random)};
                edgePending = chooseEdge(random);
                if (edgePending) {
                    switch (randomSide(random)) {
                    case 0: target.x = 0; break;
                    case 1: target.x = width - 1; break;
                    case 2: target.y = 0; break;
                    case 3: target.y = height - 1; break;
                    }
                }
                steps = randomSteps(random);
                step = 0;
            }
        }
        if (move) {
            const double t = static_cast<double>(++step) / steps;
            const double eased = t * t * (3.0 - 2.0 * t);
            current.x = static_cast<LONG>(std::lround(start.x + (target.x - start.x) * eased));
            current.y = static_cast<LONG>(std::lround(start.y + (target.y - start.y) * eased));
            if (!SetCursorPos(current.x, current.y)) {
                std::cerr << "Could not move the cursor.\n";
                result = 1;
                running = false;
            }
        }
    }
    KillTimer(nullptr, timer);
    mouseLock.stop();
    cursor.stop();
    borders.show(false);
    if (result == 0) std::cout << "Animation finished.\n";
    return result;
}
