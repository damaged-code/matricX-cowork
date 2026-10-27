// matricX-cowork.cpp — Computer-control tool for the M-Cowork AI client (Windows)
//
// Runs as a child process of the client. The client sends one JSON object per line on stdin
// ({"id":1,"cmd":"mouse_move","x":500,"y":300}) and gets one JSON reply per line on stdout
// ({"id":1,"ok":true,...}). While a session is running the screen shows a pulsing glow border,
// a HUD pill and the agent cursor so the user can always see that the AI is in control.
// Pressing the physical Escape key at any time aborts the session and gives control back.
//
// Compile: g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -static matricX-cowork.cpp -o matricX-cowork.exe -luser32 -lgdi32 -lgdiplus -lwinmm

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define OEMRESOURCE
#include <windows.h>
#include <mmsystem.h>
#include <objidl.h>
#include <gdiplus.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ============================================================================
// Constants & Version
// ============================================================================

static const char* VERSION_STRING = "matricX-cowork 0.4.0";
static const UINT_PTR TIMER_ID = 1;
static const int RENDER_INTERVAL_MS = 50;  // overlay refresh (~20 fps)
static const UINT WM_APP_STOP = WM_APP + 1;

// ============================================================================
// Enums & global state
// ============================================================================

enum class Speed { Slow, Normal, Fast };
enum class GlowTheme { Green, Cyan, Rainbow, Off };

enum StopReason { StopNone = 0, StopEscape, StopIdle, StopInputClosed, StopEndSession, StopConsole };

static std::atomic<bool> g_running{true};
static std::atomic<int> g_stopReason{StopNone};
static volatile LONG g_cursorChanged = 0;

static int g_screenW = 0;
static int g_screenH = 0;

// CLI options
static Speed g_speed = Speed::Normal;
static GlowTheme g_glowTheme = GlowTheme::Green;
static bool g_noCursor = false;
static bool g_noLock = false;
static int g_idleTimeoutSec = 120;  // 0 = never

// Timing
static LARGE_INTEGER g_startTime;
static LARGE_INTEGER g_perfFreq;
static std::atomic<ULONGLONG> g_lastActivityMs{0};

// Glow overlay
static HWND g_overlayHwnd = nullptr;
static HDC g_overlayDC = nullptr;
static HBITMAP g_overlayBmp = nullptr;
static HBITMAP g_overlayOldBmp = nullptr;
static uint32_t* g_overlayPixels = nullptr;
static std::vector<float> g_falloffMap;

// Hooks
static HHOOK g_mouseHook = nullptr;
static HHOOK g_keyHook = nullptr;

// RNG (command thread only)
static std::mt19937 g_rng;

// Tiny RAII wrappers around CRITICAL_SECTION
struct CsLock {
    CRITICAL_SECTION cs;
    CsLock() { InitializeCriticalSection(&cs); }
    ~CsLock() { DeleteCriticalSection(&cs); }
};
struct Guard {
    CRITICAL_SECTION& cs;
    explicit Guard(CsLock& l) : cs(l.cs) { EnterCriticalSection(&cs); }
    ~Guard() { LeaveCriticalSection(&cs); }
};
static CsLock g_outLock;
static CsLock g_statusLock;

// HUD status text (written by the command thread, read by the render timer)
static std::wstring g_status = L"ready";

static void setStatus(const wchar_t* s) {
    Guard g(g_statusLock);
    g_status = s;
}
static std::wstring getStatus() {
    Guard g(g_statusLock);
    return g_status;
}

static void requestStop(int reason) {
    int expected = StopNone;
    g_stopReason.compare_exchange_strong(expected, reason);
    g_running = false;
    if (g_overlayHwnd) PostMessageW(g_overlayHwnd, WM_APP_STOP, 0, 0);
}

static const char* stopReasonName(int r) {
    switch (r) {
        case StopEscape: return "escape";
        case StopIdle: return "idle_timeout";
        case StopInputClosed: return "input_closed";
        case StopEndSession: return "end_session";
        case StopConsole: return "console_signal";
        default: return "unknown";
    }
}

// ============================================================================
// Output (stdout is the reply channel — always one JSON object per line)
// ============================================================================

static void emitLine(const std::string& line) {
    Guard g(g_outLock);
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

// ============================================================================
// Minimal flat-JSON parser (objects of strings / numbers / booleans / null)
// ============================================================================

struct JVal {
    enum T { Null, Bool, Num, Str } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
};
using JObj = std::map<std::string, JVal>;

static void appendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
        o.push_back((char)cp);
    } else if (cp < 0x800) {
        o.push_back((char)(0xC0 | (cp >> 6)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back((char)(0xE0 | (cp >> 12)));
        o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        o.push_back((char)(0xF0 | (cp >> 18)));
        o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}

    bool parse(JObj& out, std::string& err) {
        skipWs();
        if (!eat('{')) {
            err = "expected a JSON object";
            return false;
        }
        skipWs();
        if (eat('}')) return finish(err);
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) {
                err = "expected a string key";
                return false;
            }
            skipWs();
            if (!eat(':')) {
                err = "expected ':' after key";
                return false;
            }
            skipWs();
            JVal v;
            if (!parseValue(v)) {
                err = "invalid or unsupported value for '" + key + "' (nested objects/arrays are not supported)";
                return false;
            }
            out[key] = v;
            skipWs();
            if (eat(',')) continue;
            if (eat('}')) break;
            err = "expected ',' or '}'";
            return false;
        }
        return finish(err);
    }

private:
    const std::string& m_s;
    size_t m_i = 0;

    bool finish(std::string& err) {
        skipWs();
        if (m_i != m_s.size()) {
            err = "unexpected trailing characters";
            return false;
        }
        return true;
    }
    void skipWs() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\r' || m_s[m_i] == '\n')) m_i++;
    }
    bool eat(char c) {
        if (m_i < m_s.size() && m_s[m_i] == c) {
            m_i++;
            return true;
        }
        return false;
    }
    bool parseLiteral(const char* lit) {
        const size_t n = strlen(lit);
        if (m_s.compare(m_i, n, lit) == 0) {
            m_i += n;
            return true;
        }
        return false;
    }
    bool parseValue(JVal& v) {
        if (m_i >= m_s.size()) return false;
        const char c = m_s[m_i];
        if (c == '"') {
            v.t = JVal::Str;
            return parseString(v.s);
        }
        if (parseLiteral("true")) { v.t = JVal::Bool; v.b = true; return true; }
        if (parseLiteral("false")) { v.t = JVal::Bool; v.b = false; return true; }
        if (parseLiteral("null")) { v.t = JVal::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            const double d = strtod(m_s.c_str() + m_i, &end);
            if (end == m_s.c_str() + m_i) return false;
            m_i = (size_t)(end - m_s.c_str());
            v.t = JVal::Num;
            v.n = d;
            return true;
        }
        return false;
    }
    bool hex4(uint32_t& v) {
        if (m_i + 4 > m_s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char h = m_s[m_i++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (uint32_t)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (uint32_t)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (uint32_t)(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool parseString(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (m_i < m_s.size()) {
            const char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_i >= m_s.size()) return false;
            const char e = m_s[m_i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                        if (m_i + 1 < m_s.size() && m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
                            m_i += 2;
                            uint32_t lo = 0;
                            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            return false;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
};

static bool getNum(const JObj& o, const char* k, double& out) {
    auto it = o.find(k);
    if (it == o.end() || it->second.t != JVal::Num) return false;
    out = it->second.n;
    return true;
}

static bool getStr(const JObj& o, const char* k, std::string& out) {
    auto it = o.find(k);
    if (it == o.end() || it->second.t != JVal::Str) return false;
    out = it->second.s;
    return true;
}

static std::string idJson(const JObj& req) {
    auto it = req.find("id");
    if (it == req.end()) return "null";
    if (it->second.t == JVal::Num) {
        char b[32];
        snprintf(b, sizeof b, "%.0f", it->second.n);
        return b;
    }
    if (it->second.t == JVal::Str) return "\"" + jsonEscape(it->second.s) + "\"";
    return "null";
}

static std::string okReply(const JObj& req, const std::string& fields = "") {
    std::string r = "{\"id\":" + idJson(req) + ",\"ok\":true";
    if (!fields.empty()) r += "," + fields;
    return r + "}";
}

static std::string errReply(const JObj& req, const std::string& msg) {
    return "{\"id\":" + idJson(req) + ",\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}";
}

// ============================================================================
// Utility
// ============================================================================

static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    int i = (int)(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

static float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static double getElapsedSeconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_startTime.QuadPart) / (double)g_perfFreq.QuadPart;
}

static float randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(g_rng);
}

static int randInt(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(g_rng);
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Sleep in small slices so a stop request interrupts long waits. Returns false if stopped.
static bool sleepMs(int ms) {
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)std::max(ms, 0);
    while (g_running) {
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return true;
        Sleep((DWORD)std::min<ULONGLONG>(end - now, 10));
    }
    return false;
}

// ============================================================================
// GraphicsRuntime — GDI+ startup/shutdown
// ============================================================================

struct GraphicsRuntime {
    ULONG_PTR token = 0;
    GraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GraphicsRuntime() {
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};

// ============================================================================
// Cursor restoration
// ============================================================================

static void restoreSystemCursors() {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    InterlockedExchange(&g_cursorChanged, 0);
}

static void atExitHandler() {
    if (InterlockedCompareExchange(&g_cursorChanged, 0, 0) != 0) {
        restoreSystemCursors();
    }
}

static BOOL WINAPI consoleHandler(DWORD) {
    requestStop(StopConsole);
    if (InterlockedCompareExchange(&g_cursorChanged, 0, 0) != 0) {
        restoreSystemCursors();
    }
    return TRUE;
}

// ============================================================================
// AgentCursor — custom cursor from akar-icons_cursor.png
// ============================================================================

struct AgentCursor {
    HCURSOR hCur = nullptr;

    static const DWORD cursorIds[14];

    void apply() {
        if (!hCur) return;
        for (int i = 0; i < 14; i++) {
            HCURSOR copy = CopyCursor(hCur);
            if (copy) {
                SetSystemCursor(copy, cursorIds[i]);
            }
        }
        InterlockedExchange(&g_cursorChanged, 1);
    }

    ~AgentCursor() {
        if (hCur) DestroyCursor(hCur);
    }
};

const DWORD AgentCursor::cursorIds[14] = {
    OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
    OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
    OCR_NO, OCR_HAND, OCR_APPSTARTING, 32651 /*OCR_HELP (undocumented value)*/
};

static HCURSOR createAgentCursor() {
    // Load akar-icons_cursor.png from the same directory as the executable
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ws(path);
    auto pos = ws.find_last_of(L"\\/");
    if (pos != std::wstring::npos) ws = ws.substr(0, pos + 1);
    ws += L"akar-icons_cursor.png";

    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromFile(ws.c_str());
    if (!src || src->GetLastStatus() != Gdiplus::Ok) {
        delete src;
        return nullptr;
    }

    // DPI-aware target size
    HDC screenDC = GetDC(nullptr);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ReleaseDC(nullptr, screenDC);
    int size = MulDiv(32, dpi, 96);

    Gdiplus::Bitmap canvas(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&canvas);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.DrawImage(src, 0, 0, size, size);
    }
    delete src;

    HBITMAP hbmp = nullptr;
    canvas.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbmp);
    if (!hbmp) return nullptr;

    HBITMAP hMask = CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hMask;
    ii.hbmColor = hbmp;

    HCURSOR hc = CreateIconIndirect(&ii);
    DeleteObject(hbmp);
    DeleteObject(hMask);
    return hc;
}

// ============================================================================
// Input hooks
//  - mouse: blocks physical mouse movement while the AI is in control (injected moves pass)
//  - keyboard: a physical Escape press aborts the session (injected Escape does not)
// ============================================================================

static LRESULT CALLBACK mouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEMOVE) {
        MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (!(ms->flags & LLMHF_INJECTED)) {
            return 1;  // block
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK keyProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb->vkCode == VK_ESCAPE && !(kb->flags & LLKHF_INJECTED)) {
            requestStop(StopEscape);
        }
    }
    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

// ============================================================================
// Human-like mouse motion
// ============================================================================

// Cubic Bézier from the current position to (tx, ty) with smoothstep easing.
// Returns false if the session was stopped mid-move.
static bool moveHuman(float tx, float ty) {
    tx = std::clamp(tx, 0.0f, (float)(g_screenW - 1));
    ty = std::clamp(ty, 0.0f, (float)(g_screenH - 1));

    POINT pt;
    GetCursorPos(&pt);
    const float sx = (float)pt.x, sy = (float)pt.y;
    const float dx = tx - sx, dy = ty - sy;
    const float len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.5f) {
        SetCursorPos((int)std::lround(tx), (int)std::lround(ty));
        return g_running;
    }

    const float ux = dx / len, uy = dy / len;
    const float px = -uy, py = ux;
    const float maxOff = std::min(len * 0.25f, 120.0f);
    const float r1 = randFloat(-1.0f, 1.0f) * maxOff;
    const float r2 = randFloat(-1.0f, 1.0f) * maxOff;

    const float p1x = sx + ux * len * 0.33f + px * r1, p1y = sy + uy * len * 0.33f + py * r1;
    const float p2x = sx + ux * len * 0.67f + px * r2, p2y = sy + uy * len * 0.67f + py * r2;

    float pixelsPerStep = 22.0f;
    if (g_speed == Speed::Slow) pixelsPerStep = 12.0f;
    if (g_speed == Speed::Fast) pixelsPerStep = 40.0f;
    const int steps = (int)std::clamp(len / pixelsPerStep * randFloat(0.85f, 1.15f), 8.0f, 120.0f);

    for (int i = 1; i <= steps; i++) {
        if (!g_running) return false;
        const float t = smoothstep((float)i / (float)steps);
        const float u = 1.0f - t;
        const float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
        float x = b0 * sx + b1 * p1x + b2 * p2x + b3 * tx;
        float y = b0 * sy + b1 * p1y + b2 * p2y + b3 * ty;
        x = std::clamp(x, 0.0f, (float)(g_screenW - 1));
        y = std::clamp(y, 0.0f, (float)(g_screenH - 1));
        SetCursorPos((int)std::lround(x), (int)std::lround(y));
        if (!sleepMs(10)) return false;
    }

    // short settle with micro-jitter, then land exactly on the target
    const int settle = randInt(40, 90);
    for (int e = 0; e < settle; e += 10) {
        SetCursorPos((int)std::lround(tx + randFloat(-0.6f, 0.6f)), (int)std::lround(ty + randFloat(-0.6f, 0.6f)));
        if (!sleepMs(10)) return false;
    }
    SetCursorPos((int)std::lround(tx), (int)std::lround(ty));
    return g_running;
}

// ============================================================================
// Synthetic input
// ============================================================================

enum class Button { Left, Right, Middle };

static void sendMouse(DWORD flags, DWORD data = 0) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(INPUT));
}

static void buttonEvent(Button b, bool down) {
    DWORD f = 0;
    switch (b) {
        case Button::Left: f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case Button::Right: f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case Button::Middle: f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    }
    sendMouse(f);
}

static bool isExtendedKey(WORD vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

static void sendKey(WORD vk, bool down) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (isExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(INPUT));
}

static void sendUnicode(wchar_t ch, bool down) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

static bool typeText(const std::string& utf8, int delayMs) {
    const std::wstring w = utf8ToWide(utf8);
    for (size_t i = 0; i < w.size(); ++i) {
        if (!g_running) return false;
        wchar_t c = w[i];
        if (c == L'\r') {
            if (i + 1 < w.size() && w[i + 1] == L'\n') continue;  // CRLF counts once
            c = L'\n';
        }
        if (c == L'\n') {
            sendKey(VK_RETURN, true);
            sendKey(VK_RETURN, false);
        } else if (c == L'\t') {
            sendKey(VK_TAB, true);
            sendKey(VK_TAB, false);
        } else {
            sendUnicode(c, true);
            sendUnicode(c, false);
        }
        if (!sleepMs(delayMs + randInt(0, delayMs / 2))) return false;
    }
    return true;
}

static bool parseKeyName(const std::string& nameIn, WORD& vk) {
    std::string n = nameIn;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    static const std::map<std::string, WORD> named = {
        {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"shift", VK_SHIFT}, {"alt", VK_MENU},
        {"win", VK_LWIN}, {"meta", VK_LWIN}, {"super", VK_LWIN}, {"cmd", VK_LWIN},
        {"enter", VK_RETURN}, {"return", VK_RETURN}, {"tab", VK_TAB},
        {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    };
    auto it = named.find(n);
    if (it != named.end()) {
        vk = it->second;
        return true;
    }
    if (n.size() >= 2 && n[0] == 'f' && std::all_of(n.begin() + 1, n.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int num = std::atoi(n.c_str() + 1);
        if (num >= 1 && num <= 24) {
            vk = (WORD)(VK_F1 + num - 1);
            return true;
        }
        return false;
    }
    wchar_t ch = 0;
    if (n == "plus") ch = L'+';
    else if (n.size() == 1) ch = (wchar_t)(unsigned char)n[0];
    if (ch != 0) {
        const SHORT r = VkKeyScanW(ch);
        if (r == -1) return false;
        vk = (WORD)(r & 0xFF);
        return true;
    }
    return false;
}

// "ctrl+shift+t" -> hold all keys, then release in reverse order
static bool pressCombo(const std::string& combo, std::string& err) {
    std::vector<WORD> vks;
    size_t start = 0;
    for (;;) {
        const size_t plus = combo.find('+', start);
        const std::string part = combo.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (part.empty()) {
            err = "empty key name in '" + combo + "' (use 'plus' for the + key)";
            return false;
        }
        WORD vk = 0;
        if (!parseKeyName(part, vk)) {
            err = "unknown key '" + part + "'";
            return false;
        }
        vks.push_back(vk);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    for (WORD vk : vks) {
        sendKey(vk, true);
        sleepMs(15);
    }
    for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
        sendKey(*it, false);
        sleepMs(15);
    }
    return true;
}

// ============================================================================
// Screenshot
// ============================================================================

static bool getEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<char> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

static bool captureScreen(const std::wstring& pathIn, std::wstring& pathOut, std::string& err) {
    std::wstring path = pathIn;
    if (path.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"matricX-cowork";
        CreateDirectoryW(dir.c_str(), nullptr);
        path = dir + L"\\screenshot-" + std::to_wstring((unsigned long long)GetTickCount64()) + L".png";
    }

    CLSID png;
    if (!getEncoderClsid(L"image/png", &png)) {
        err = "PNG encoder not available";
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, g_screenW, g_screenH);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = BitBlt(mem, 0, 0, g_screenW, g_screenH, screen, 0, 0, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);

    bool saved = false;
    if (ok) {
        Gdiplus::Bitmap bitmap(bmp, nullptr);
        saved = bitmap.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        err = "screen capture failed";
        return false;
    }
    if (!saved) {
        err = "could not save the PNG (check the path)";
        return false;
    }
    pathOut = path;
    return true;
}

// ============================================================================
// Overlay window (glow + HUD)
// ============================================================================

static const wchar_t* OVERLAY_CLASS = L"MatricXCoworkOverlay";

static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_APP_STOP:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void buildFalloffMap() {
    int w = g_screenW, h = g_screenH;
    g_falloffMap.resize((size_t)w * h);

    const double spread = std::clamp(std::min(w, h) * 0.075, 40.0, 140.0);
    const auto glow = [spread](double distance) {
        double d = distance / spread;
        return std::exp(-d * d * 2.0);
    };

    for (int y = 0; y < h; y++) {
        double vertical = (1.0 - glow(y)) * (1.0 - glow(h - 1 - y));
        for (int x = 0; x < w; x++) {
            double fade = 1.0 - vertical * (1.0 - glow(x)) * (1.0 - glow(w - 1 - x));
            float val = static_cast<float>(fade);
            if (val < 0.003f) val = 0.0f;
            g_falloffMap[(size_t)y * w + x] = val;
        }
    }
}

static void createOverlay() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    g_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS, L"",
        WS_POPUP,
        0, 0, g_screenW, g_screenH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );

    // keep the glow/HUD out of screenshots (best effort, Windows 10 2004+)
    SetWindowDisplayAffinity(g_overlayHwnd, WDA_EXCLUDEFROMCAPTURE);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_screenW;
    bmi.bmiHeader.biHeight = -g_screenH;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(nullptr);
    g_overlayDC = CreateCompatibleDC(screenDC);
    g_overlayBmp = CreateDIBSection(g_overlayDC, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&g_overlayPixels), nullptr, 0);
    g_overlayOldBmp = (HBITMAP)SelectObject(g_overlayDC, g_overlayBmp);
    ReleaseDC(nullptr, screenDC);

    buildFalloffMap();

    ShowWindow(g_overlayHwnd, SW_SHOWNOACTIVATE);
}

static void destroyOverlay() {
    if (g_overlayDC) {
        SelectObject(g_overlayDC, g_overlayOldBmp);
        DeleteObject(g_overlayBmp);
        DeleteDC(g_overlayDC);
        g_overlayDC = nullptr;
    }
    if (g_overlayHwnd) {
        DestroyWindow(g_overlayHwnd);
        g_overlayHwnd = nullptr;
    }
    UnregisterClassW(OVERLAY_CLASS, GetModuleHandleW(nullptr));
}

static void addRoundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, float radius) {
    float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void renderOverlay(double elapsed) {
    if (!g_overlayHwnd || !g_overlayPixels) return;
    int w = g_screenW, h = g_screenH;
    size_t totalPixels = (size_t)w * h;

    float pulse = 0.7f + 0.3f * (float)std::sin(elapsed * 1.8);

    float cr = 0.0f, cg = 0.0f, cb = 0.0f;
    float mix = 0.5f + 0.5f * (float)std::sin(elapsed * 0.7);

    switch (g_glowTheme) {
        case GlowTheme::Green:
            cr = 0.12f * (1.0f - mix * 0.3f);
            cg = 0.94f;
            cb = 0.48f + mix * 0.15f;
            break;
        case GlowTheme::Cyan:
            cr = 0.0f; cg = 0.75f; cb = 0.95f;
            break;
        case GlowTheme::Rainbow: {
            float hue = (float)std::fmod(elapsed * 0.08, 1.0);
            hsvToRgb(hue, 1.0f, 1.0f, cr, cg, cb);
            break;
        }
        case GlowTheme::Off:
            break;
    }

    uint8_t rr = (uint8_t)(cr * 255.0f);
    uint8_t gg = (uint8_t)(cg * 255.0f);
    uint8_t bb = (uint8_t)(cb * 255.0f);

    for (size_t i = 0; i < totalPixels; i++) {
        float f = g_falloffMap[i];
        if (f <= 0.0f || g_glowTheme == GlowTheme::Off) {
            g_overlayPixels[i] = 0;
            continue;
        }
        uint8_t a = (uint8_t)(f * pulse * 0.72f * 255.0f);
        uint8_t pr = (uint8_t)((rr * a) / 255);  // premultiplied alpha for UpdateLayeredWindow
        uint8_t pg = (uint8_t)((gg * a) / 255);
        uint8_t pb = (uint8_t)((bb * a) / 255);
        g_overlayPixels[i] = ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | pb;
    }

    // HUD pill
    {
        Gdiplus::Graphics gfx(g_overlayDC);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        std::wstring hudText = L"ESC to stop │ AI: " + getStatus();

        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font font(&fontFamily, 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        Gdiplus::RectF textBounds;
        gfx.MeasureString(hudText.c_str(), -1, &font, Gdiplus::PointF(0, 0), &textBounds);

        float padX = 10.0f, padY = 5.0f;
        float pillW = textBounds.Width + padX * 2.0f;
        float pillH = textBounds.Height + padY * 2.0f;
        float pillX = 10.0f, pillY = 10.0f;

        Gdiplus::RectF pillRect(pillX, pillY, pillW, pillH);

        Gdiplus::GraphicsPath pillPath;
        addRoundedRect(pillPath, pillRect, 6.0f);
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(200, 10, 10, 10));
        gfx.FillPath(&bgBrush, &pillPath);

        uint8_t br_r = rr, br_g = gg, br_b = bb;
        if (g_glowTheme == GlowTheme::Off) { br_r = 100; br_g = 100; br_b = 100; }
        Gdiplus::Pen borderPen(Gdiplus::Color(200, br_r, br_g, br_b), 1.0f);
        gfx.DrawPath(&borderPen, &pillPath);

        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
        gfx.DrawString(hudText.c_str(), -1, &font,
                       Gdiplus::PointF(pillX + padX, pillY + padY), &textBrush);
    }

    POINT ptSrc = {0, 0};
    POINT ptDst = {0, 0};
    SIZE sz = {(LONG)w, (LONG)h};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_overlayHwnd, nullptr, &ptDst, &sz, g_overlayDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

static void CALLBACK timerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_running) return;
    if (g_idleTimeoutSec > 0) {
        const ULONGLONG idle = GetTickCount64() - g_lastActivityMs.load();
        if (idle > (ULONGLONG)g_idleTimeoutSec * 1000ULL) {
            requestStop(StopIdle);
            return;
        }
    }
    renderOverlay(getElapsedSeconds());
}

// ============================================================================
// Command handling
// ============================================================================

static std::string cursorFields() {
    POINT pt;
    GetCursorPos(&pt);
    return "\"x\":" + std::to_string(pt.x) + ",\"y\":" + std::to_string(pt.y);
}

// returns 1 = point given, 0 = not given, -1 = invalid (err set)
static int getPoint(const JObj& req, const char* kx, const char* ky, float& x, float& y, std::string& err) {
    double dx = 0, dy = 0;
    const bool hx = getNum(req, kx, dx), hy = getNum(req, ky, dy);
    if (hx != hy) {
        err = std::string("'") + kx + "' and '" + ky + "' must be given together";
        return -1;
    }
    if (!hx) return 0;
    x = (float)dx;
    y = (float)dy;
    return 1;
}

static std::string dispatch(const JObj& req, const std::string& cmd, bool& endSession) {
    std::string err;


    if (cmd == "fs_list") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' (directory path) is required");
        std::error_code ec;
        const std::filesystem::path dir(rawPath);
        if (!std::filesystem::is_directory(dir, ec) || ec) return errReply(req, "path is not a readable directory");
        std::string entries = "\"entries\":[";
        size_t count = 0;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && count < 200; it.increment(ec)) {
            if (count++) entries += ",";
            const auto name = it->path().filename().u8string();
            const bool isDir = it->is_directory(ec);
            const auto size = isDir ? 0ull : it->file_size(ec);
            if (ec) { ec.clear(); }
            entries += "{\"name\":\"" + jsonEscape(name) + "\",\"directory\":" + (isDir ? "true" : "false") +
                       ",\"size\":" + std::to_string(size) + "}";
        }
        if (ec) return errReply(req, "failed while listing directory");
        return okReply(req, entries + "]");
    }

    if (cmd == "fs_read") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' (file path) is required");
        std::error_code ec;
        const std::filesystem::path file(rawPath);
        if (!std::filesystem::is_regular_file(file, ec) || ec) return errReply(req, "path is not a readable regular file");
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size > 1048576) return errReply(req, "file exceeds the 1 MiB read limit or cannot be sized");
        std::ifstream in(file, std::ios::binary);
        if (!in) return errReply(req, "cannot open file for reading");
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.eof() && in.fail()) return errReply(req, "failed while reading file");
        return okReply(req, "\"path\":\"" + jsonEscape(file.u8string()) + "\",\"content\":\"" + jsonEscape(content) +
                            "\",\"bytes\":" + std::to_string(content.size()));
    }

    if (cmd == "fs_write") {
        std::string rawPath, content;
        if (!getStr(req, "path", rawPath) || !getStr(req, "content", content))
            return errReply(req, "'path' and 'content' (strings) are required");
        if (content.size() > 1048576) return errReply(req, "content exceeds the 1 MiB write limit");
        const auto itOverwrite = req.find("overwrite");
        const bool overwrite = itOverwrite != req.end() && itOverwrite->second.t == JVal::Bool && itOverwrite->second.b;
        const std::filesystem::path file(rawPath);
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && !overwrite) return errReply(req, "file already exists; set overwrite=true to replace it");
        if (ec) return errReply(req, "cannot inspect target path");
        if (!file.parent_path().empty() && !std::filesystem::is_directory(file.parent_path(), ec))
            return errReply(req, "parent directory does not exist");
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return errReply(req, "cannot open file for writing");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) return errReply(req, "failed while writing file");
        return okReply(req, "\"path\":\"" + jsonEscape(file.u8string()) + "\",\"bytes_written\":" + std::to_string(content.size()));
    }

    if (cmd == "fs_mkdir") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' (directory path) is required");
        std::error_code ec;
        const bool created = std::filesystem::create_directories(std::filesystem::path(rawPath), ec);
        if (ec) return errReply(req, "cannot create directory: " + ec.message());
        return okReply(req, "\"path\":\"" + jsonEscape(rawPath) + "\",\"created\":" + (created ? "true" : "false"));
    }

    if (cmd == "fs_delete") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' is required");
        const std::filesystem::path target(rawPath);
        if (target.empty() || target == target.root_path() || target == target.root_path().root_path())
            return errReply(req, "refusing to delete an empty or root path");
        std::error_code ec;
        if (!std::filesystem::exists(target, ec) || ec) return errReply(req, "target does not exist");
        if (std::filesystem::is_directory(target, ec)) return errReply(req, "directory deletion is disabled; delete files individually");
        const bool removed = std::filesystem::remove(target, ec);
        if (ec || !removed) return errReply(req, "failed to delete file");
        return okReply(req, "\"path\":\"" + jsonEscape(target.u8string()) + "\",\"deleted\":true");
    }

    if (cmd == "screen_info") {
        POINT p;
        GetCursorPos(&p);
        return okReply(req, "\"width\":" + std::to_string(g_screenW) + ",\"height\":" + std::to_string(g_screenH) +
                                ",\"cursor_x\":" + std::to_string(p.x) + ",\"cursor_y\":" + std::to_string(p.y));
    }

    if (cmd == "mouse_move") {
        float x = 0, y = 0;
        const int r = getPoint(req, "x", "y", x, y, err);
        if (r < 0) return errReply(req, err);
        if (r == 0) return errReply(req, "'x' and 'y' are required");
        setStatus(L"moving");
        if (!moveHuman(x, y)) return errReply(req, "aborted");
        return okReply(req, cursorFields());
    }

    if (cmd == "mouse_click") {
        float x = 0, y = 0;
        const int r = getPoint(req, "x", "y", x, y, err);
        if (r < 0) return errReply(req, err);

        Button button = Button::Left;
        std::string b;
        if (getStr(req, "button", b)) {
            if (b == "left") button = Button::Left;
            else if (b == "right") button = Button::Right;
            else if (b == "middle") button = Button::Middle;
            else return errReply(req, "'button' must be left, right or middle");
        }
        double countD = 1;
        getNum(req, "count", countD);
        const int count = (int)std::clamp(countD, 1.0, 3.0);

        setStatus(L"clicking");
        if (r == 1 && !moveHuman(x, y)) return errReply(req, "aborted");
        if (!sleepMs(randInt(60, 120))) return errReply(req, "aborted");
        for (int i = 0; i < count; i++) {
            buttonEvent(button, true);
            const bool up1 = sleepMs(randInt(35, 80));
            buttonEvent(button, false);  // always release
            if (!up1) return errReply(req, "aborted");
            if (i + 1 < count && !sleepMs(randInt(70, 110))) return errReply(req, "aborted");
        }
        return okReply(req, cursorFields());
    }

    if (cmd == "mouse_drag") {
        float x = 0, y = 0, fx = 0, fy = 0;
        const int r = getPoint(req, "x", "y", x, y, err);
        if (r < 0) return errReply(req, err);
        if (r == 0) return errReply(req, "'x' and 'y' (drop point) are required");
        const int rf = getPoint(req, "from_x", "from_y", fx, fy, err);
        if (rf < 0) return errReply(req, err);

        setStatus(L"dragging");
        if (rf == 1 && !moveHuman(fx, fy)) return errReply(req, "aborted");
        if (!sleepMs(randInt(60, 110))) return errReply(req, "aborted");
        buttonEvent(Button::Left, true);
        bool ok = sleepMs(randInt(70, 120)) && moveHuman(x, y) && sleepMs(randInt(60, 110));
        buttonEvent(Button::Left, false);  // always release
        if (!ok) return errReply(req, "aborted");
        return okReply(req, cursorFields());
    }

    if (cmd == "mouse_scroll") {
        double dxN = 0, dyN = 0;
        getNum(req, "dx", dxN);
        getNum(req, "dy", dyN);
        if (dxN == 0 && dyN == 0) return errReply(req, "give a non-zero 'dy' (vertical) and/or 'dx' (horizontal), in wheel notches");
        float x = 0, y = 0;
        const int r = getPoint(req, "x", "y", x, y, err);
        if (r < 0) return errReply(req, err);

        setStatus(L"scrolling");
        if (r == 1 && !moveHuman(x, y)) return errReply(req, "aborted");
        const int vy = (int)std::clamp(dyN, -50.0, 50.0);
        const int hx = (int)std::clamp(dxN, -50.0, 50.0);
        for (int i = 0; i < std::abs(vy); i++) {
            sendMouse(MOUSEEVENTF_WHEEL, (DWORD)(int)(vy > 0 ? -WHEEL_DELTA : WHEEL_DELTA));  // positive dy = down
            if (!sleepMs(25)) return errReply(req, "aborted");
        }
        for (int i = 0; i < std::abs(hx); i++) {
            sendMouse(MOUSEEVENTF_HWHEEL, (DWORD)(int)(hx > 0 ? WHEEL_DELTA : -WHEEL_DELTA));  // positive dx = right
            if (!sleepMs(25)) return errReply(req, "aborted");
        }
        return okReply(req, cursorFields());
    }

    if (cmd == "keyboard_type") {
        std::string text;
        if (!getStr(req, "text", text)) return errReply(req, "'text' (string) is required");
        if (text.size() > 20000) return errReply(req, "'text' is too long (max 20000 bytes)");
        double delay = 12;
        getNum(req, "delay_ms", delay);
        setStatus(L"typing");
        if (!typeText(text, (int)std::clamp(delay, 0.0, 500.0))) return errReply(req, "aborted");
        return okReply(req);
    }

    if (cmd == "keyboard_key") {
        std::string keys;
        if (!getStr(req, "keys", keys)) return errReply(req, "'keys' (string, e.g. \"ctrl+c\" or \"ctrl+a ctrl+c\") is required");
        setStatus(L"pressing keys");
        size_t start = 0;
        bool any = false;
        while (start < keys.size()) {
            while (start < keys.size() && keys[start] == ' ') start++;
            if (start >= keys.size()) break;
            size_t sp = keys.find(' ', start);
            const std::string combo = keys.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
            if (!pressCombo(combo, err)) return errReply(req, err);
            any = true;
            if (!sleepMs(randInt(30, 70))) return errReply(req, "aborted");
            if (sp == std::string::npos) break;
            start = sp + 1;
        }
        if (!any) return errReply(req, "'keys' is empty");
        return okReply(req);
    }

    if (cmd == "screenshot") {
        std::string p;
        getStr(req, "path", p);
        setStatus(L"looking");
        std::wstring outPath;
        if (!captureScreen(utf8ToWide(p), outPath, err)) return errReply(req, err);
        return okReply(req, "\"path\":\"" + jsonEscape(wideToUtf8(outPath)) + "\",\"width\":" + std::to_string(g_screenW) +
                                ",\"height\":" + std::to_string(g_screenH));
    }

    if (cmd == "wait") {
        double ms = 0;
        if (!getNum(req, "ms", ms)) return errReply(req, "'ms' (number) is required");
        setStatus(L"waiting");
        if (!sleepMs((int)std::clamp(ms, 0.0, 60000.0))) return errReply(req, "aborted");
        return okReply(req);
    }

    if (cmd == "end_session") {
        endSession = true;
        return okReply(req);
    }

    return errReply(req, cmd.empty() ? "missing 'cmd'" : "unknown command '" + cmd + "'");
}

static DWORD WINAPI commandThread(LPVOID) {
    std::string line;
    for (;;) {
        line.clear();
        int c = 0;
        while ((c = fgetc(stdin)) != EOF && c != '\n') line.push_back((char)c);
        if (c == EOF && line.empty()) break;  // stdin closed
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        JObj req;
        std::string perr;
        if (!JsonParser(line).parse(req, perr)) {
            emitLine("{\"id\":null,\"ok\":false,\"error\":\"bad request: " + jsonEscape(perr) + "\"}");
            continue;
        }
        if (!g_running) {
            emitLine(errReply(req, "session stopped"));
            continue;
        }

        g_lastActivityMs = GetTickCount64();
        std::string cmd;
        getStr(req, "cmd", cmd);
        bool endSession = false;
        const std::string reply = dispatch(req, cmd, endSession);
        g_lastActivityMs = GetTickCount64();
        setStatus(L"ready");
        emitLine(reply);

        if (endSession) {
            requestStop(StopEndSession);
            break;
        }
    }
    requestStop(StopInputClosed);
    return 0;
}

// ============================================================================
// CLI
// ============================================================================

// Tool schemas in the format AI APIs expect (name / description / input_schema)
static const char* TOOL_DEFINITIONS = R"JSON([
  {"name":"screen_info","description":"Get the screen size in pixels and the current mouse position. All coordinates used by the other tools are in these pixels, origin at the top-left corner.","input_schema":{"type":"object","properties":{},"required":[]}},
  {"name":"screenshot","description":"Capture the whole screen to a PNG file and return its path, width and height. The AI glow overlay is not part of the capture.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Optional output path ending in .png. Defaults to a file in the temp folder."}},"required":[]}},
  {"name":"mouse_move","description":"Move the mouse cursor to (x, y) along a smooth, human-like path.","input_schema":{"type":"object","properties":{"x":{"type":"integer","description":"Target X in screen pixels"},"y":{"type":"integer","description":"Target Y in screen pixels"}},"required":["x","y"]}},
  {"name":"mouse_click","description":"Click a mouse button. If x and y are given the cursor first moves there, otherwise the click happens at the current position.","input_schema":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"button":{"type":"string","enum":["left","right","middle"],"description":"Default left"},"count":{"type":"integer","minimum":1,"maximum":3,"description":"1 = click, 2 = double-click, 3 = triple-click. Default 1"}},"required":[]}},
  {"name":"mouse_drag","description":"Press the left button, move to (x, y) and release. Starts at (from_x, from_y) if given, otherwise at the current position.","input_schema":{"type":"object","properties":{"x":{"type":"integer","description":"Drop X"},"y":{"type":"integer","description":"Drop Y"},"from_x":{"type":"integer"},"from_y":{"type":"integer"}},"required":["x","y"]}},
  {"name":"mouse_scroll","description":"Scroll the mouse wheel. dy > 0 scrolls down, dy < 0 up; dx > 0 scrolls right, dx < 0 left. Values are wheel notches. If x and y are given the cursor moves there first.","input_schema":{"type":"object","properties":{"dy":{"type":"integer"},"dx":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"}},"required":[]}},
  {"name":"keyboard_type","description":"Type text into the focused window like a person typing. Newlines press Enter and tabs press Tab.","input_schema":{"type":"object","properties":{"text":{"type":"string"},"delay_ms":{"type":"integer","description":"Average delay between characters, 0-500. Default 12"}},"required":["text"]}},
  {"name":"keyboard_key","description":"Press a key or shortcut such as \"enter\", \"ctrl+c\" or \"alt+tab\". Several shortcuts can be given separated by spaces, e.g. \"ctrl+a ctrl+c\". Names: ctrl, shift, alt, win, enter, tab, esc, space, backspace, delete, insert, home, end, pageup, pagedown, up, down, left, right, f1-f24, plus single characters (a, 5, ...; use \"plus\" for the + key).","input_schema":{"type":"object","properties":{"keys":{"type":"string"}},"required":["keys"]}},
  {"name":"wait","description":"Pause for a number of milliseconds (max 60000), e.g. to let a page or app finish loading.","input_schema":{"type":"object","properties":{"ms":{"type":"integer"}},"required":["ms"]}},
  {"name":"fs_list","description":"List up to 200 entries in a directory. Returns names, directory flags, and file sizes.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path to list"}},"required":["path"]}},
  {"name":"fs_read","description":"Read a UTF-8/text file up to 1 MiB. Returns its content and byte count.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path to read"}},"required":["path"]}},
  {"name":"fs_write","description":"Create a text file up to 1 MiB. Existing files are preserved unless overwrite=true is explicitly supplied.","input_schema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"},"overwrite":{"type":"boolean","description":"Must be true to replace an existing file"}},"required":["path","content"]}},
  {"name":"fs_mkdir","description":"Create a directory and any missing parent directories.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path to create"}},"required":["path"]}},
  {"name":"fs_delete","description":"Delete one file. Directory deletion and root-path deletion are refused.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path to delete"}},"required":["path"]}},
  {"name":"end_session","description":"Finish: remove the overlay, restore the normal cursor and give mouse control back to the user. Call this when the task is done.","input_schema":{"type":"object","properties":{},"required":[]}}
])JSON";

static void printHelp() {
    printf(
        "matricX-cowork - computer-control tool for the M-Cowork AI client\n\n"
        "Usage: matricX-cowork [options]\n\n"
        "Protocol: one JSON object per line on stdin, one JSON reply per line on stdout.\n"
        "  -> {\"id\":1,\"cmd\":\"mouse_move\",\"x\":500,\"y\":300}\n"
        "  <- {\"id\":1,\"ok\":true,\"x\":500,\"y\":300}\n"
        "Events (no id): {\"event\":\"ready\",...} on start, {\"event\":\"stopped\",\"reason\":...} on exit.\n"
        "Run with --describe to list every command and its parameters.\n\n"
        "Options:\n"
        "  --speed <mode>        slow | normal | fast (default normal)\n"
        "  --glow <theme>        green | cyan | rainbow | off (default green)\n"
        "  --idle-timeout <sec>  End the session after this long without commands (default 120, 0 = never)\n"
        "  --no-cursor           Skip custom cursor replacement\n"
        "  --no-lock             Don't block manual mouse movement\n"
        "  --describe            Print the tool definitions as JSON and exit\n"
        "  --restore-cursor      Restore default Windows cursors and exit\n"
        "  --help                Show this help\n"
        "  --version             Show version\n\n"
        "Press the physical Escape key at any time to abort the session.\n"
    );
}

// returns false when the program should exit right away (exitCode set)
static bool parseArgs(int argc, char** argv, int& exitCode) {
    exitCode = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printHelp();
            return false;
        }
        if (arg == "--version") {
            printf("%s\n", VERSION_STRING);
            return false;
        }
        if (arg == "--describe") {
            fputs(TOOL_DEFINITIONS, stdout);
            fputc('\n', stdout);
            return false;
        }
        if (arg == "--restore-cursor") {
            restoreSystemCursors();
            printf("System cursors restored.\n");
            return false;
        }
        if (arg == "--no-cursor") {
            g_noCursor = true;
            continue;
        }
        if (arg == "--no-lock") {
            g_noLock = true;
            continue;
        }
        if (arg == "--idle-timeout" && i + 1 < argc) {
            g_idleTimeoutSec = std::max(0, std::atoi(argv[++i]));
            continue;
        }
        if (arg == "--speed" && i + 1 < argc) {
            std::string val = argv[++i];
            if (val == "slow") g_speed = Speed::Slow;
            else if (val == "fast") g_speed = Speed::Fast;
            else g_speed = Speed::Normal;
            continue;
        }
        if (arg == "--glow" && i + 1 < argc) {
            std::string val = argv[++i];
            if (val == "cyan") g_glowTheme = GlowTheme::Cyan;
            else if (val == "rainbow") g_glowTheme = GlowTheme::Rainbow;
            else if (val == "off") g_glowTheme = GlowTheme::Off;
            else g_glowTheme = GlowTheme::Green;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        printHelp();
        exitCode = 2;
        return false;
    }
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    int exitCode = 0;
    if (!parseArgs(argc, argv, exitCode)) return exitCode;

    std::atexit(atExitHandler);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    SetProcessDPIAware();
    timeBeginPeriod(1);  // 1 ms timer resolution for smooth Sleep(10) steps

    g_rng.seed((unsigned)std::time(nullptr) ^ (unsigned)GetTickCount());

    GraphicsRuntime gdiRuntime;

    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    AgentCursor agentCursor;
    if (!g_noCursor) {
        agentCursor.hCur = createAgentCursor();
        if (agentCursor.hCur) agentCursor.apply();
    }

    createOverlay();

    if (!g_noLock) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, nullptr, 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, nullptr, 0);

    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startTime);
    g_lastActivityMs = GetTickCount64();

    renderOverlay(0.0);
    SetTimer(g_overlayHwnd, TIMER_ID, RENDER_INTERVAL_MS, timerProc);

    emitLine(std::string("{\"event\":\"ready\",\"version\":\"") + VERSION_STRING + "\",\"width\":" +
             std::to_string(g_screenW) + ",\"height\":" + std::to_string(g_screenH) + "}");

    HANDLE cmdThread = CreateThread(nullptr, 0, commandThread, nullptr, 0, nullptr);
    if (cmdThread) CloseHandle(cmdThread);  // detached; ends when stdin closes or the process exits

    if (g_running) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Cleanup
    g_running = false;
    if (g_overlayHwnd) KillTimer(g_overlayHwnd, TIMER_ID);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    g_mouseHook = nullptr;
    g_keyHook = nullptr;
    destroyOverlay();

    if (InterlockedCompareExchange(&g_cursorChanged, 0, 0) != 0) {
        restoreSystemCursors();
    }

    emitLine(std::string("{\"event\":\"stopped\",\"reason\":\"") + stopReasonName(g_stopReason.load()) + "\"}");
    timeEndPeriod(1);
    return 0;
}
