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
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

static const char* VERSION_STRING = "matricX-cowork 0.5.0";
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
static bool g_shellEnabled = false;  // shell_exec requires an explicit opt-in
static std::vector<std::string> g_allowRoots;  // extra roots for fs_* / shell_exec

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
    enum T { Null, Bool, Num, Str, Arr } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> arr;  // flat arrays only (strings/numbers/bools/null)
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
    bool parseScalar(JVal& v) {
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
    bool parseValue(JVal& v) {
        if (m_i >= m_s.size()) return false;
        if (m_s[m_i] == '[') {
            // Flat array: strings / numbers / bools / null. Nested objects and arrays
            // are still unsupported (the computer-control protocol stays flat).
            v.t = JVal::Arr;
            m_i++;
            skipWs();
            if (eat(']')) return true;
            for (;;) {
                skipWs();
                if (m_i < m_s.size() && (m_s[m_i] == '[' || m_s[m_i] == '{')) return false;
                JVal el;
                if (!parseScalar(el)) return false;
                v.arr.push_back(el);
                skipWs();
                if (eat(',')) continue;
                if (eat(']')) break;
                return false;
            }
            return true;
        }
        return parseScalar(v);
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

static bool getBool(const JObj& o, const char* k, bool def) {
    auto it = o.find(k);
    if (it == o.end() || it->second.t != JVal::Bool) return def;
    return it->second.b;
}

static std::vector<std::string> getArgv(const JObj& o) {
    std::vector<std::string> out;
    auto it = o.find("argv");
    if (it == o.end() || it->second.t != JVal::Arr) return out;
    for (const auto& el : it->second.arr) {
        if (el.t == JVal::Str) out.push_back(el.s);
    }
    return out;
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
// Filesystem & shell tool sandbox
// ============================================================================

static const size_t kFsMaxBytes = 1048576;      // hard cap for fs_read payloads
static const size_t kShellMaxOutput = 262144;   // 256 KiB combined output cap
static const size_t kWriteMaxBytes = 2097152;   // hard cap for fs_write payloads
static const int kShellMaxArgs = 100;
static const size_t kShellMaxArgBytes = 8192;
static const char* kIgnoredDirs[] = {".git", "node_modules", "__pycache__", ".venv", "venv",
                                     "target", "dist", "Build", "Debug", "Release", ".cache"};

static bool isIgnoredDirName(const std::string& name) {
    for (const char* n : kIgnoredDirs) {
        if (name == n) return true;
    }
    return false;
}

static void defaultRoots(std::vector<std::string>& out) {
    char home[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        out.push_back(home);
    }
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) {
        std::wstring dir(tmp);
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        out.push_back(wideToUtf8(dir));
    }
}

static void allRoots(std::vector<std::string>& out) {
    defaultRoots(out);
    for (const std::string& r : g_allowRoots) out.push_back(r);
}

static fs::path toPath(const std::string& s) {
    return fs::path(utf8ToWide(s));
}

static std::string pathToUtf8(const fs::path& p) {
    return wideToUtf8(p.wstring());
}

// Resolves a client-supplied path and verifies it stays inside an allowed root.
static bool ensureAllowed(const std::string& pathIn, fs::path& canonOut, std::string& err) {
    if (pathIn.empty()) {
        err = "'path' is required";
        return false;
    }
    fs::path p = toPath(pathIn);
    if (p.is_relative()) {
        std::vector<std::string> roots;
        defaultRoots(roots);
        if (!roots.empty()) p = toPath(roots.front()) / p;
    }
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) {
        err = "cannot resolve path '" + pathIn + "': " + ec.message();
        return false;
    }
    std::vector<std::string> roots;
    allRoots(roots);
    for (const std::string& rootStr : roots) {
        std::error_code rec;
        fs::path root = fs::weakly_canonical(toPath(rootStr), rec);
        if (rec || root.empty()) continue;
        const std::wstring cn = canon.wstring();
        const std::wstring rn = root.wstring();
        // Windows comparisons ignore case and accept either separator.
        auto isPrefix = [](const std::wstring& full, const std::wstring& pre) {
            if (pre.empty() || full.size() < pre.size()) return false;
            for (size_t i = 0; i < pre.size(); ++i) {
                wchar_t a = full[i] == L'/' ? L'\\' : full[i];
                wchar_t b = pre[i] == L'/' ? L'\\' : pre[i];
                if (towlower(a) != towlower(b)) return false;
            }
            return full.size() == pre.size();
        };
        auto isPrefixPlusSlash = [&](const std::wstring& full, const std::wstring& pre) {
            if (pre.empty() || full.size() <= pre.size()) return false;
            wchar_t sep = full[pre.size()] == L'/' ? L'\\' : full[pre.size()];
            return sep == L'\\' && isPrefix(full.substr(0, pre.size()), pre);
        };
        if (isPrefix(cn, rn) || isPrefixPlusSlash(cn, rn)) {
            canonOut = canon;
            return true;
        }
    }
    err = "path '" + pathIn + "' is outside the allowed roots (start matricX-cowork with --allow-root <path> to extend)";
    return false;
}

static void appendJsonStr(std::string& out, const std::string& s) {
    out += '"';
    out += jsonEscape(s);
    out += '"';
}

static uintmax_t entrySize(const fs::path& p, std::error_code& ec) {
    return fs::file_size(p, ec);
}

static long long entryMtime(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    const long long epochOffset = 116444736000000000LL;  // 1601 → 1970
    long long ft = ((long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return (ft - epochOffset) / 10000000LL;
}

static std::string listDirectoryReply(const JObj& req, const fs::path& dir, int maxEntries) {
    struct Ent {
        std::string name;
        int type;  // 0 dir, 1 file, 2 symlink, 3 other
        uintmax_t size = 0;
        long long mtime = 0;
    };
    std::vector<Ent> entries;
    std::error_code ec;
    bool truncated = false;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        Ent e;
        e.name = pathToUtf8(it->path().filename());
        std::error_code sec;
        fs::file_status st = it->symlink_status(sec);
        if (ec || sec) continue;
        if (fs::is_symlink(st)) e.type = 2;
        else if (fs::is_directory(st)) e.type = 0;
        else if (fs::is_regular_file(st)) e.type = 1;
        else e.type = 3;
        if (e.type == 1) {
            std::error_code fec;
            uintmax_t sz = entrySize(it->path(), fec);
            if (!fec) e.size = sz;
        }
        e.mtime = entryMtime(it->path());
        entries.push_back(e);
    }
    if (entries.size() > (size_t)maxEntries) {
        // C++17 lacks stable directory order; sort with lightweight case-insensitive compare.
        std::sort(entries.begin(), entries.end(), [](const Ent& a, const Ent& b) {
            if (a.type != b.type) {
                if (a.type == 0) return true;
                if (b.type == 0) return false;
                return a.type < b.type;
            }
            return _wcsicmp(utf8ToWide(a.name).c_str(), utf8ToWide(b.name).c_str()) < 0;
        });
        entries.resize(maxEntries);
        truncated = true;
    }
    std::string arr = "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const Ent& e = entries[i];
        if (i) arr += ",";
        std::string typeStr = (e.type == 0) ? "dir" : (e.type == 1) ? "file" : (e.type == 2) ? "symlink" : "other";
        arr += "{\"name\":";
        appendJsonStr(arr, e.name);
        arr += ",\"type\":\"" + typeStr + "\"";
        if (e.type == 1) arr += ",\"size\":" + std::to_string(e.size);
        arr += ",\"mtime\":" + std::to_string(e.mtime) + "}";
    }
    arr += "]";
    return okReply(req, "\"entries\":" + arr + ",\"truncated\":" + (truncated ? "true" : "false"));
}

static bool readFileReply(const JObj& req, std::string& fields, std::string& err) {
    std::string p;
    if (!getStr(req, "path", p)) {
        err = "'path' is required";
        return false;
    }
    double maxB = 65536;
    getNum(req, "max_bytes", maxB);
    const size_t cap = (size_t)std::clamp(maxB, 1.0, (double)kFsMaxBytes);

    fs::path canon;
    if (!ensureAllowed(p, canon, err)) return false;

    std::error_code ec;
    if (!fs::is_regular_file(canon, ec)) {
        err = "not a regular file: " + p;
        return false;
    }
    uintmax_t fsize = fs::file_size(canon, ec);
    if (ec) fsize = 0;

    std::ifstream in(canon, std::ios::binary);
    if (!in) {
        err = "cannot open file: " + p;
        return false;
    }
    std::vector<char> buf(cap);
    in.read(buf.data(), (std::streamsize)cap);
    const size_t got = (size_t)in.gcount();
    buf.resize(got);
    const bool truncated = (fsize > cap) || (got == cap && !in.eof());

    bool binary = false;
    for (char c : buf) {
        if (c == '\0') { binary = true; break; }
    }

    fields = "\"path\":";
    appendJsonStr(fields, pathToUtf8(canon));
    fields += ",\"size\":" + std::to_string(fsize);
    fields += ",\"truncated\":";
    fields += truncated ? "true" : "false";
    if (binary) {
        static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((got + 2) / 3) * 4);
        for (size_t i = 0; i < got; i += 3) {
            unsigned v = (unsigned char)buf[i] << 16;
            if (i + 1 < got) v |= (unsigned char)buf[i + 1] << 8;
            if (i + 2 < got) v |= (unsigned char)buf[i + 2];
            out += kAlpha[(v >> 18) & 63];
            out += kAlpha[(v >> 12) & 63];
            out += (i + 1 < got) ? kAlpha[(v >> 6) & 63] : '=';
            out += (i + 2 < got) ? kAlpha[v & 63] : '=';
        }
        fields += ",\"binary\":true,\"content_base64\":";
        appendJsonStr(fields, out);
    } else {
        fields += ",\"content\":";
        appendJsonStr(fields, std::string(buf.data(), buf.size()));
    }
    return true;
}

static bool writeFileReply(const JObj& req, std::string& fields, std::string& err) {
    std::string p, text, b64;
    if (!getStr(req, "path", p)) {
        err = "'path' is required";
        return false;
    }
    getStr(req, "text", text);
    getStr(req, "base64", b64);
    std::vector<unsigned char> data;
    if (!b64.empty()) {
        static const std::map<char, int> rev = [] {
            std::map<char, int> m;
            const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; i++) m[alpha[i]] = i;
            return m;
        }();
        int acc = 0, bits = 0;
        for (char c : b64) {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
            auto it = rev.find(c);
            if (it == rev.end()) {
                err = "'base64' contains an invalid character";
                return false;
            }
            acc = (acc << 6) | it->second;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                data.push_back((unsigned char)((acc >> bits) & 0xFF));
            }
        }
    } else if (!text.empty() || req.find("text") != req.end()) {
        data.assign(text.begin(), text.end());
    }
    if (data.empty() && text.empty() && b64.empty()) {
        err = "either 'text' or 'base64' is required";
        return false;
    }
    if (data.size() > kWriteMaxBytes) {
        err = "write payload is too large (max " + std::to_string(kWriteMaxBytes) + " bytes)";
        return false;
    }

    fs::path canon;
    if (!ensureAllowed(p, canon, err)) return false;

    const bool overwrite = getBool(req, "overwrite", true);
    std::error_code ec;
    if (fs::exists(canon, ec) && !overwrite) {
        err = "file already exists; pass overwrite=true to replace it";
        return false;
    }
    char tmpSuffix[32];
    snprintf(tmpSuffix, sizeof tmpSuffix, ".mcow-tmp-%lu-%llu", GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    const std::string tmp = pathToUtf8(canon) + tmpSuffix;
    {
        std::ofstream out(toPath(tmp), std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot create file (check permissions)";
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        out.flush();
        if (!out) {
            out.close();
            DeleteFileW(toPath(tmp).c_str());
            err = "failed while writing file contents";
            return false;
        }
    }
    if (fs::exists(canon, ec)) DeleteFileW(canon.c_str());
    fs::rename(toPath(tmp), canon, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(toPath(tmp), ec2);
        err = "cannot finalize the write: " + ec.message();
        return false;
    }
    fields = "\"path\":";
    appendJsonStr(fields, pathToUtf8(canon));
    fields += ",\"bytes\":" + std::to_string(data.size());
    return true;
}

static bool mkdirReply(const JObj& req, std::string& fields, std::string& err) {
    std::string p;
    if (!getStr(req, "path", p)) {
        err = "'path' is required";
        return false;
    }
    fs::path canon;
    if (!ensureAllowed(p, canon, err)) return false;
    std::error_code ec;
    const bool existed = fs::exists(canon, ec);
    if (existed && !fs::is_directory(canon, ec)) {
        err = "a non-directory item already exists at that path";
        return false;
    }
    if (!existed) {
        fs::create_directories(canon, ec);
        if (ec) {
            err = "cannot create directory: " + ec.message();
            return false;
        }
    }
    fields = "\"path\":";
    appendJsonStr(fields, pathToUtf8(canon));
    fields += ",\"created\":";
    fields += existed ? "false" : "true";
    return true;
}

static bool deletePathReply(const JObj& req, std::string& fields, std::string& err) {
    std::string p;
    if (!getStr(req, "path", p)) {
        err = "'path' is required";
        return false;
    }
    const bool recursive = getBool(req, "recursive", false);

    fs::path canon;
    if (!ensureAllowed(p, canon, err)) return false;

    // Never allow removing an allowed root itself.
    std::vector<std::string> roots;
    allRoots(roots);
    for (const std::string& rootStr : roots) {
        std::error_code rec;
        fs::path root = fs::weakly_canonical(toPath(rootStr), rec);
        if (!rec && _wcsicmp(canon.c_str(), root.c_str()) == 0) {
            err = "refusing to delete an allowed root directory";
            return false;
        }
    }

    std::error_code ec;
    fs::file_status st = fs::symlink_status(canon, ec);
    if (ec) {
        err = "cannot inspect path: " + ec.message();
        return false;
    }
    bool didDelete = false;
    if (fs::is_directory(st) && !fs::is_symlink(st)) {
        if (!recursive) {
            err = "refusing to delete a directory without recursive=true";
            return false;
        }
        fs::remove_all(canon, ec);
        if (ec) {
            err = "cannot delete directory: " + ec.message();
            return false;
        }
        didDelete = true;
    } else if (!fs::exists(st) && !fs::is_symlink(st)) {
        err = "path does not exist: " + p;
        return false;
    } else {
        fs::remove(canon, ec);
        if (ec) {
            err = "cannot delete: " + ec.message();
            return false;
        }
        didDelete = true;
    }
    fields = "\"deleted\":";
    fields += didDelete ? "true" : "false";
    return true;
}

// Iterative wildcard matching supporting * and ? (no escapes).
static bool wildcardMatch(const std::string& sIn, const std::string& patIn, bool caseSensitive) {
    std::string s = sIn, pat = patIn;
    if (!caseSensitive) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
        std::transform(pat.begin(), pat.end(), pat.begin(), [](unsigned char c) { return (char)tolower(c); });
    }
    size_t si = 0, pi = 0, star = std::string::npos, mark = 0;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) {
            si++;
            pi++;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') pi++;
    return pi == pat.size();
}

static bool searchReply(const JObj& req, std::string& fields, std::string& err) {
    std::string base, patternIn;
    double maxD = 100;
    getNum(req, "max_results", maxD);
    int maxResults = (int)std::clamp(maxD, 1.0, 200.0);
    getStr(req, "path", base);
    if (!getStr(req, "pattern", patternIn) || patternIn.empty()) {
        err = "'pattern' is required (supports * and ?)";
        return false;
    }
    fs::path canon;
    std::error_code ec;
    if (!base.empty()) {
        if (!ensureAllowed(base, canon, err)) return false;
    } else {
        std::vector<std::string> roots;
        defaultRoots(roots);
        if (roots.empty()) {
            err = "no allowed root available";
            return false;
        }
        canon = fs::weakly_canonical(toPath(roots.front()), ec);
    }
    if (!fs::is_directory(canon, ec)) {
        err = "not a directory: " + base;
        return false;
    }

    const bool caseSensitive = false;  // Windows names are case-insensitive

    std::vector<std::string> matches;
    bool truncated = false;
    std::vector<std::pair<fs::path, int>> stack;
    stack.emplace_back(canon, 0);
    while (!stack.empty() && !truncated) {
        const fs::path dirPath = stack.back().first;
        const int depth = stack.back().second;
        stack.pop_back();
        std::error_code ec;
        for (fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            std::error_code sec;
            fs::path child = it->path();
            fs::file_status st = it->symlink_status(sec);
            if (ec || sec) continue;
            const std::string name = pathToUtf8(child.filename());
            if (!name.empty() && name[0] == '.') continue;  // skip hidden entries
            if (wildcardMatch(name, patternIn, caseSensitive)) {
                matches.push_back(pathToUtf8(child));
                if ((int)matches.size() >= maxResults) {
                    truncated = true;
                    break;
                }
            }
            if (fs::is_directory(st) && depth < 8 && !fs::is_symlink(st) && !isIgnoredDirName(name)) {
                stack.emplace_back(child, depth + 1);
            }
        }
    }
    std::string arr = "[";
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) arr += ",";
        appendJsonStr(arr, matches[i]);
    }
    arr += "]";
    fields = "\"matches\":" + arr;
    fields += ",\"truncated\":";
    fields += truncated ? "true" : "false";
    return true;
}

// Windows: run an argv program with pipes, timeout and capped captured output.
static bool runShellWin(const std::vector<std::string>& args, const std::string& cwd,
                        int timeoutMs, const std::string& stdinData, int& exitCode, bool& timedOut,
                        std::string& out, std::string& errOut, std::string& error) {
    if (args.empty() || args[0].empty()) {
        error = "'argv' must be a non-empty array of strings";
        return false;
    }
    if ((int)args.size() > kShellMaxArgs) {
        error = "too many argv entries (max " + std::to_string(kShellMaxArgs) + ")";
        return false;
    }
    size_t total = 0;
    for (const std::string& a : args) total += a.size();
    if (total > 16 * kShellMaxArgBytes) {
        error = "argv is too large";
        return false;
    }

    // Build a quoted command line (MSVC rules, simplified \" escaping).
    std::wstring cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdLine += L" ";
        std::wstring w = utf8ToWide(args[i]);
        const bool needsQuote = w.empty() || w.find_first_of(L" \t\"") != std::wstring::npos;
        if (needsQuote) cmdLine += L"\"";
        for (wchar_t ch : w) {
            if (ch == L'"') cmdLine += L"\\\"";
            else cmdLine += ch;
        }
        if (needsQuote) cmdLine += L"\"";
    }

    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0) ||
        !CreatePipe(&inR, &inW, &sa, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    // Keep only the child's ends inheritable.
    SetHandleInformation(outW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(nullptr, &cmdLine[0], &sa, &sa, TRUE, 0, nullptr,
                             cwd.empty() ? nullptr : toPath(cwd).c_str(), &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    CloseHandle(inR);
    if (!ok) {
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(outR);
        CloseHandle(errR);
        CloseHandle(inW);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        return false;
    }
    CloseHandle(pi.hThread);

    if (!stdinData.empty()) {
        DWORD written = 0;
        WriteFile(inW, stdinData.data(), (DWORD)stdinData.size(), &written, nullptr);
    }
    CloseHandle(inW);

    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
    auto drain = [&](HANDLE h, std::string& dst) {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) return;
            char tmp[8192];
            DWORD n = (DWORD)std::min<DWORD>(avail, sizeof tmp);
            DWORD got = 0;
            if (!ReadFile(h, tmp, n, &got, nullptr) || got == 0) return;
            if (dst.size() < kShellMaxOutput) {
                dst.append(tmp, (size_t)std::min<DWORD>(got, (DWORD)(kShellMaxOutput - dst.size())));
            }
        }
    };

    for (;;) {
        drain(outR, out);
        drain(errR, errOut);
        if (GetTickCount64() >= deadline) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 20) != WAIT_TIMEOUT) {
            drain(outR, out);
            drain(errR, errOut);
            break;
        }
    }
    CloseHandle(outR);
    CloseHandle(errR);

    if (timedOut || WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    WaitForSingleObject(pi.hProcess, 5000);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    exitCode = (int)code;
    return true;
}

static bool shellExecReply(const JObj& req, std::string& fields, std::string& err) {
    if (!g_shellEnabled) {
        err = "shell execution is disabled. Enable 'Allow shell command execution' in M-Cowork settings.";
        return false;
    }
    std::vector<std::string> args = getArgv(req);
    if (args.empty()) {
        err = "'argv' (array of strings, no shell string) is required";
        return false;
    }
    for (const std::string& a : args) {
        if (a.size() > kShellMaxArgBytes) {
            err = "an argv entry exceeds the size limit";
            return false;
        }
    }
    std::string cwd;
    getStr(req, "cwd", cwd);
    if (!cwd.empty()) {
        fs::path canon;
        if (!ensureAllowed(cwd, canon, err)) return false;
        std::error_code ec;
        if (!fs::is_directory(canon, ec)) {
            err = "cwd is not a directory: " + cwd;
            return false;
        }
        cwd = pathToUtf8(canon);
    }
    double timeoutD = 60000;
    getNum(req, "timeout_ms", timeoutD);
    const int timeoutMs = (int)std::clamp(timeoutD, 1.0, 300000.0);
    std::string stdinStr;
    getStr(req, "stdin", stdinStr);

    int exitCode = 0;
    bool timedOut = false;
    std::string out, errOut, error;
    if (!runShellWin(args, cwd, timeoutMs, stdinStr, exitCode, timedOut, out, errOut, error)) {
        err = error;
        return false;
    }
    fields = "\"argv\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) fields += ",";
        appendJsonStr(fields, args[i]);
    }
    fields += "],\"exit_code\":" + std::to_string(exitCode);
    fields += ",\"timed_out\":";
    fields += timedOut ? "true" : "false";
    fields += ",\"stdout\":";
    appendJsonStr(fields, out);
    fields += ",\"stderr\":";
    appendJsonStr(fields, errOut);
    return true;
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

    if (cmd == "fs_list" || cmd == "fs_read" || cmd == "fs_write" || cmd == "fs_mkdir" ||
        cmd == "fs_delete" || cmd == "fs_search" || cmd == "shell_exec") {
        setStatus(L"filesystem");
        if (cmd == "fs_list") {
            std::string p;
            if (!getStr(req, "path", p) || p.empty()) return errReply(req, "'path' is required");
            fs::path canon;
            if (!ensureAllowed(p, canon, err)) return errReply(req, err);
            std::error_code ec;
            if (!fs::is_directory(canon, ec)) return errReply(req, "not a directory: " + p);
            double maxD = 200;
            getNum(req, "max_entries", maxD);
            const int maxEntries = (int)std::clamp(maxD, 1.0, 500.0);
            return listDirectoryReply(req, canon, maxEntries);
        }
        if (cmd == "fs_read") {
            std::string fields;
            if (!readFileReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
        if (cmd == "fs_write") {
            std::string fields;
            if (!writeFileReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
        if (cmd == "fs_mkdir") {
            std::string fields;
            if (!mkdirReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
        if (cmd == "fs_delete") {
            std::string fields;
            if (!deletePathReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
        if (cmd == "fs_search") {
            std::string fields;
            if (!searchReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
        if (cmd == "shell_exec") {
            std::string fields;
            if (!shellExecReply(req, fields, err)) return errReply(req, err);
            return okReply(req, fields);
        }
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
  {"name":"fs_list","description":"List the entries of a directory (name, type, size, mtime). Paths must stay inside an allowed root (the user's home and temp folder, or roots given via --allow-root).","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path (absolute, or relative to the home root)"},"max_entries":{"type":"integer","description":"Optional cap, default 200, max 500"}},"required":["path"]}},
  {"name":"fs_read","description":"Read a text or binary file and return its content (content, or content_base64 for binary). Reads are capped in size and limited to allowed roots.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"max_bytes":{"type":"integer","description":"Optional byte cap, default 65536, max 1048576"}},"required":["path"]}},
  {"name":"fs_write","description":"Create or overwrite a file. Pass text as 'text' (UTF-8) or raw bytes as 'base64'. Overwriting requires overwrite=true unless the file is new.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path to create or replace"},"text":{"type":"string","description":"UTF-8 content to write"},"base64":{"type":"string","description":"Optional base64 content for binary files"},"overwrite":{"type":"boolean","description":"Replace an existing file. Default true"}},"required":["path"]}},
  {"name":"fs_mkdir","description":"Create a directory (and any missing parents). Limited to allowed roots.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path to create"}},"required":["path"]}},
  {"name":"fs_delete","description":"Delete a file or empty directory. Directory contents require recursive=true. Never use on paths the user may need. Limited to allowed roots.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Path to delete"},"recursive":{"type":"boolean","description":"Must be true to delete directories non-empty contents. Default false"}},"required":["path"]}},
  {"name":"fs_search","description":"Search file names under a directory tree with a wildcard pattern (* and ?). Hidden files, dependency folders (.git, node_modules, ...) are skipped.","input_schema":{"type":"object","properties":{"pattern":{"type":"string","description":"Wildcard pattern, e.g. *.txt or report-*.md"},"path":{"type":"string","description":"Optional base directory, default the home root"},"max_results":{"type":"integer","description":"Optional cap, default 100, max 200"}},"required":["pattern"]}},
  {"name":"shell_exec","description":"Execute a program with an argv array (never a shell string) and return exit code, stdout and stderr. Only available when the user enabled shell tools in settings; requires a timeout (max 300000 ms) and returns at most 256 KiB of combined output. Optional 'stdin' feeds data to the process. Optional 'cwd' must be inside an allowed root.","input_schema":{"type":"object","properties":{"argv":{"type":"array","items":{"type":"string"},"description":"Program plus arguments, e.g. [\"cmd\",\"/c\",\"dir\"]"},"cwd":{"type":"string","description":"Optional working directory (inside allowed roots)"},"timeout_ms":{"type":"integer","description":"Default 60000, max 300000"},"stdin":{"type":"string","description":"Optional data piped to the program's stdin"}},"required":["argv"]}},
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
        "  --enable-shell        Allow the shell_exec tool (off by default)\n"
        "  --allow-root <path>   Extend the fs/shell allowed roots (default: home and temp)\n"
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
        if (arg == "--enable-shell") {
            g_shellEnabled = true;
            continue;
        }
        if (arg == "--allow-root" && i + 1 < argc) {
            std::string root = argv[++i];
            if (!root.empty()) g_allowRoots.push_back(root);
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
