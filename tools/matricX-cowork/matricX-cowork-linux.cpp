// matricX-cowork-linux.cpp — Computer-control tool for the M-Cowork AI client (Linux / X11)
//
// Linux port of matricX-cowork.
// Communicates with client via JSON-lines over stdin/stdout.
// Supports X11/XTest for input automation, root window screenshot to PNG via libpng,
// and smooth Bézier curve mouse motion. Gracefully handles headless/no-display environments.
//
// Compile: g++ -std=c++17 -O2 -Wall -Wextra matricX-cowork-linux.cpp -o matricX-cowork -lX11 -lXtst -lXfixes -lpng -lpthread

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;


#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <png.h>

#ifdef Bool
#undef Bool
#endif
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef Success
#undef Success
#endif

// ============================================================================
// Constants & Version
// ============================================================================

static const char* VERSION_STRING = "matricX-cowork 0.5.0 (Linux/X11)";

enum class Speed { Slow, Normal, Fast };
enum class GlowTheme { Green, Cyan, Rainbow, Off };
enum StopReason { StopNone = 0, StopEscape, StopIdle, StopInputClosed, StopEndSession, StopConsole };

static std::atomic<bool> g_running{true};
static std::atomic<int> g_stopReason{StopNone};

static int g_screenW = 1920;
static int g_screenH = 1080;

// CLI options
static Speed g_speed = Speed::Normal;
static GlowTheme g_glowTheme = GlowTheme::Green;
static bool g_noCursor = false;
static bool g_noLock = false;
static int g_idleTimeoutSec = 120;  // 0 = never
static bool g_shellEnabled = false;  // shell_exec requires an explicit opt-in
static std::vector<std::string> g_allowRoots;  // extra roots for fs_* / shell_exec

// Timing
static std::chrono::steady_clock::time_point g_startTime;
static std::atomic<uint64_t> g_lastActivityMs{0};

// Synchronization
static std::mutex g_outMutex;
static std::mutex g_statusMutex;
static std::mutex g_x11Mutex;
static std::string g_status = "ready";

// Display connection
static Display* g_display = nullptr;
static int g_screen = 0;
static Window g_root = 0;

// RNG
static std::mt19937 g_rng;

static uint64_t getNowMs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = s;
}

static std::string getStatus() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

static void requestStop(int reason) {
    int expected = StopNone;
    g_stopReason.compare_exchange_strong(expected, reason);
    g_running = false;
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
// Output (stdout is the reply channel — one JSON object per line)
// ============================================================================

static void emitLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
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
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

// ============================================================================
// Minimal JSON Parser
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
        const size_t n = std::strlen(lit);
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
            const double d = std::strtod(m_s.c_str() + m_i, &end);
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
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
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
        std::snprintf(b, sizeof b, "%.0f", it->second.n);
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
// Math & Timing Utilities
// ============================================================================

static float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(g_rng);
}

static int randInt(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(g_rng);
}

static bool sleepMs(int ms) {
    const uint64_t end = getNowMs() + (uint64_t)std::max(ms, 0);
    while (g_running) {
        const uint64_t now = getNowMs();
        if (now >= end) return true;
        uint64_t remain = end - now;
        uint64_t step = std::min<uint64_t>(remain, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
    }
    return false;
}

// ============================================================================
// X11 Helpers & Input Simulation
// ============================================================================

static void getCursorPos(int& x, int& y) {
    if (!g_display) {
        x = 0; y = 0;
        return;
    }
    std::lock_guard<std::mutex> lock(g_x11Mutex);
    Window rootReturn, childReturn;
    int winX, winY;
    unsigned int mask;
    if (XQueryPointer(g_display, g_root, &rootReturn, &childReturn, &x, &y, &winX, &winY, &mask)) {
        return;
    }
    x = 0; y = 0;
}

static void setCursorPos(int x, int y) {
    if (!g_display) return;
    std::lock_guard<std::mutex> lock(g_x11Mutex);
    XTestFakeMotionEvent(g_display, g_screen, x, y, CurrentTime);
    XFlush(g_display);
}

static bool moveHuman(float tx, float ty) {
    tx = std::clamp(tx, 0.0f, (float)(g_screenW - 1));
    ty = std::clamp(ty, 0.0f, (float)(g_screenH - 1));

    int curX = 0, curY = 0;
    getCursorPos(curX, curY);
    const float sx = (float)curX, sy = (float)curY;
    const float dx = tx - sx, dy = ty - sy;
    const float len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.5f) {
        setCursorPos((int)std::lround(tx), (int)std::lround(ty));
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
        setCursorPos((int)std::lround(x), (int)std::lround(y));
        if (!sleepMs(10)) return false;
    }

    const int settle = randInt(40, 90);
    for (int e = 0; e < settle; e += 10) {
        setCursorPos((int)std::lround(tx + randFloat(-0.6f, 0.6f)), (int)std::lround(ty + randFloat(-0.6f, 0.6f)));
        if (!sleepMs(10)) return false;
    }
    setCursorPos((int)std::lround(tx), (int)std::lround(ty));
    return g_running;
}

enum class Button { Left, Right, Middle };

static void buttonEvent(Button b, bool down) {
    if (!g_display) return;
    std::lock_guard<std::mutex> lock(g_x11Mutex);
    unsigned int btn = 1; // Left
    if (b == Button::Middle) btn = 2;
    else if (b == Button::Right) btn = 3;
    XTestFakeButtonEvent(g_display, btn, down ? True : False, CurrentTime);
    XFlush(g_display);
}

static void sendWheel(int button, int count) {
    if (!g_display) return;
    std::lock_guard<std::mutex> lock(g_x11Mutex);
    for (int i = 0; i < count; ++i) {
        XTestFakeButtonEvent(g_display, button, True, CurrentTime);
        XTestFakeButtonEvent(g_display, button, False, CurrentTime);
    }
    XFlush(g_display);
}

static bool parseKeyNameX11(const std::string& nameIn, KeySym& sym) {
    std::string n = nameIn;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    static const std::map<std::string, KeySym> named = {
        {"ctrl", XK_Control_L}, {"control", XK_Control_L}, {"shift", XK_Shift_L}, {"alt", XK_Alt_L},
        {"win", XK_Super_L}, {"meta", XK_Super_L}, {"super", XK_Super_L}, {"cmd", XK_Super_L},
        {"enter", XK_Return}, {"return", XK_Return}, {"tab", XK_Tab},
        {"esc", XK_Escape}, {"escape", XK_Escape}, {"space", XK_space},
        {"backspace", XK_BackSpace}, {"delete", XK_Delete}, {"del", XK_Delete}, {"insert", XK_Insert},
        {"home", XK_Home}, {"end", XK_End},
        {"pageup", XK_Page_Up}, {"pgup", XK_Page_Up}, {"pagedown", XK_Page_Down}, {"pgdn", XK_Page_Down},
        {"up", XK_Up}, {"down", XK_Down}, {"left", XK_Left}, {"right", XK_Right},
    };
    auto it = named.find(n);
    if (it != named.end()) {
        sym = it->second;
        return true;
    }
    if (n.size() >= 2 && n[0] == 'f' && std::all_of(n.begin() + 1, n.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        int num = std::atoi(n.c_str() + 1);
        if (num >= 1 && num <= 24) {
            sym = (KeySym)(XK_F1 + num - 1);
            return true;
        }
        return false;
    }
    if (n == "plus") {
        sym = XK_plus;
        return true;
    }
    if (n.size() == 1) {
        KeySym s = XStringToKeysym(n.c_str());
        if (s != NoSymbol) {
            sym = s;
            return true;
        }
    }
    return false;
}

static void sendKeySym(KeySym sym, bool down) {
    if (!g_display) return;
    std::lock_guard<std::mutex> lock(g_x11Mutex);
    KeyCode code = XKeysymToKeycode(g_display, sym);
    if (code != 0) {
        XTestFakeKeyEvent(g_display, code, down ? True : False, CurrentTime);
        XFlush(g_display);
    }
}

static bool pressCombo(const std::string& combo, std::string& err) {
    std::vector<KeySym> syms;
    size_t start = 0;
    for (;;) {
        const size_t plus = combo.find('+', start);
        const std::string part = combo.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (part.empty()) {
            err = "empty key name in '" + combo + "' (use 'plus' for the + key)";
            return false;
        }
        KeySym sym = NoSymbol;
        if (!parseKeyNameX11(part, sym)) {
            err = "unknown key '" + part + "'";
            return false;
        }
        syms.push_back(sym);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    for (KeySym sym : syms) {
        sendKeySym(sym, true);
        sleepMs(15);
    }
    for (auto it = syms.rbegin(); it != syms.rend(); ++it) {
        sendKeySym(*it, false);
        sleepMs(15);
    }
    return true;
}

static bool typeText(const std::string& utf8, int delayMs) {
    if (!g_display) return true;
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (!g_running) return false;
        char c = utf8[i];
        if (c == '\r') {
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') continue;
            c = '\n';
        }
        if (c == '\n') {
            sendKeySym(XK_Return, true);
            sendKeySym(XK_Return, false);
        } else if (c == '\t') {
            sendKeySym(XK_Tab, true);
            sendKeySym(XK_Tab, false);
        } else {
            char str[2] = {c, 0};
            KeySym sym = XStringToKeysym(str);
            if (sym != NoSymbol) {
                sendKeySym(sym, true);
                sendKeySym(sym, false);
            }
        }
        if (!sleepMs(delayMs + randInt(0, std::max(1, delayMs / 2)))) return false;
    }
    return true;
}

// ============================================================================
// Screen Capture to PNG (libpng)
// ============================================================================

static bool savePngImage(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); std::fclose(fp); return false; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); std::fclose(fp); return false; }

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<const uint8_t*> rowPointers(height);
    for (int y = 0; y < height; ++y) {
        rowPointers[y] = rgba.data() + (size_t)y * width * 4;
    }
    png_write_image(png, const_cast<png_bytepp>(rowPointers.data()));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

static int x11ErrorHandler(Display*, XErrorEvent*) {
    // Suppress BadMatch / BadWindow errors on synthetic or rootless windows
    return 0;
}

static bool captureScreen(const std::string& pathIn, std::string& pathOut, std::string& err) {
    std::string path = pathIn;
    if (path.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        std::string dir = std::string(tmp) + "/matricX-cowork";
        mkdir(dir.c_str(), 0755);
        path = dir + "/screenshot-" + std::to_string(getNowMs()) + ".png";
    }

    // 1. If running under Wayland, grim provides native, non-rootless desktop capture
    if (std::getenv("WAYLAND_DISPLAY")) {
        std::string grimCmd = "grim \"" + path + "\" 2>/dev/null";
        if (std::system(grimCmd.c_str()) == 0) {
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
                pathOut = path;
                return true;
            }
        }
    }

    // 2. Try XGetImage on X11 root window
    if (g_display) {
        std::lock_guard<std::mutex> lock(g_x11Mutex);
        XImage* image = XGetImage(g_display, g_root, 0, 0, g_screenW, g_screenH, AllPlanes, ZPixmap);
        if (image) {
            std::vector<uint8_t> rgba((size_t)g_screenW * g_screenH * 4);
            for (int y = 0; y < g_screenH; ++y) {
                for (int x = 0; x < g_screenW; ++x) {
                    unsigned long pixel = XGetPixel(image, x, y);
                    uint8_t r = (uint8_t)((pixel & image->red_mask) >> 16);
                    uint8_t g = (uint8_t)((pixel & image->green_mask) >> 8);
                    uint8_t b = (uint8_t)(pixel & image->blue_mask);
                    size_t idx = ((size_t)y * g_screenW + x) * 4;
                    rgba[idx + 0] = r;
                    rgba[idx + 1] = g;
                    rgba[idx + 2] = b;
                    rgba[idx + 3] = 255;
                }
            }
            XDestroyImage(image);

            if (savePngImage(path, g_screenW, g_screenH, rgba)) {
                pathOut = path;
                return true;
            }
        }
    }

    // 3. Fallback: try grim or import CLI if present
    std::string tryGrim = "grim \"" + path + "\" 2>/dev/null";
    if (std::system(tryGrim.c_str()) == 0) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            pathOut = path;
            return true;
        }
    }

    std::string tryImport = "import -window root \"" + path + "\" 2>/dev/null";
    if (std::system(tryImport.c_str()) == 0) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            pathOut = path;
            return true;
        }
    }

    // 4. Graceful headless fallback
    std::vector<uint8_t> dummy((size_t)g_screenW * g_screenH * 4, 30);
    if (!savePngImage(path, g_screenW, g_screenH, dummy)) {
        err = "could not write PNG file to " + path;
        return false;
    }
    pathOut = path;
    return true;
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
    const char* home = std::getenv("HOME");
    if (home && *home) out.push_back(home);
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    out.push_back(tmp);
}

static void allRoots(std::vector<std::string>& out) {
    defaultRoots(out);
    for (const std::string& r : g_allowRoots) out.push_back(r);
}

static fs::path toPath(const std::string& s) {
#ifdef _WIN32
    return fs::path(s);
#else
    return fs::path(s);
#endif
}

static std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
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
        const std::string cn = pathToUtf8(canon);
        const std::string rn = pathToUtf8(root);
        if (cn == rn) {
            canonOut = canon;
            return true;
        }
        if (cn.size() > rn.size() && cn.compare(0, rn.size(), rn) == 0 &&
            (rn.back() == '/' || cn[rn.size()] == '/')) {
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

// Portable mtime helper: engine-specific file time conversion.
static long long entryMtime(const fs::path& p) {
    struct stat st;
    if (::stat(p.string().c_str(), &st) != 0) return 0;
    return (long long)st.st_mtime;
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
        if (ec) continue;
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
    std::sort(entries.begin(), entries.end(), [](const Ent& a, const Ent& b) {
        if (a.type != b.type) {
            if (a.type == 0) return true;
            if (b.type == 0) return false;
            return a.type < b.type;
        }
        return a.name < b.name;
    });
    if ((int)entries.size() > maxEntries) {
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
    } else if (getStr(req, "text", text) || req.find("text") != req.end()) {
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
    const std::string tmp = canon.string() + ".mcow-tmp-" + std::to_string((long long)getpid()) + "-" + std::to_string(getNowMs());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot create file (check permissions)";
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        out.flush();
        if (!out) {
            out.close();
            std::remove(tmp.c_str());
            err = "failed while writing file contents";
            return false;
        }
    }
    if (fs::exists(canon, ec)) std::remove(canon.string().c_str());
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
    const std::string cn = pathToUtf8(canon);
    for (const std::string& rootStr : roots) {
        std::error_code rec;
        fs::path root = fs::weakly_canonical(toPath(rootStr), rec);
        if (!rec && cn == pathToUtf8(root)) {
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
        uintmax_t n = fs::remove_all(canon, ec);
        if (ec) {
            err = "cannot delete directory: " + ec.message();
            return false;
        }
        (void)n;
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
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(pat.begin(), pat.end(), pat.begin(), [](unsigned char c) { return (char)std::tolower(c); });
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
        canon = fs::weakly_canonical(toPath(roots.front()), ec);
    }
    if (!fs::is_directory(canon, ec)) {
        err = "not a directory: " + base;
        return false;
    }

#ifdef _WIN32
    const bool caseSensitive = false;
#else
    const bool caseSensitive = true;
#endif

    std::vector<std::string> matches;
    bool truncated = false;
    std::vector<std::pair<fs::path, int>> stack;
    stack.emplace_back(canon, 0);
    while (!stack.empty() && !truncated) {
        const auto [dirPath, depth] = stack.back();
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
    fields = "\"matches\":" + arr + (truncated ? ",\"truncated\":true" : ",\"truncated\":false");
    return true;
}

// POSIX: run argv with pipes, timeout and capped captured output.
static bool runShellPosix(const std::vector<std::string>& args, const std::string& cwd,
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

    int outPipe[2], errPipe[2], inPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0) {
        error = "pipe() failed";
        return false;
    }

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork() failed";
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        close(inPipe[0]); close(inPipe[1]);
        return false;
    }
    if (pid == 0) {
        fcntl(outPipe[1], F_SETFD, 0);  // keep write end for the child
        fcntl(errPipe[1], F_SETFD, 0);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        if (!stdinData.empty()) {
            dup2(inPipe[0], STDIN_FILENO);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        }
        // Close everything except the new std streams.
        int maxfd = (int)sysconf(_SC_OPEN_MAX) > 1024 ? 1024 : (int)sysconf(_SC_OPEN_MAX);
        for (int fd = 0; fd < maxfd; ++fd) {
            if (fd == STDOUT_FILENO || fd == STDERR_FILENO || fd == STDIN_FILENO) continue;
            close(fd);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        std::vector<char*> argvC;
        argvC.reserve(args.size() + 1);
        for (const std::string& a : args) argvC.push_back(const_cast<char*>(a.c_str()));
        argvC.push_back(nullptr);
        execvp(argvC[0], argvC.data());
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);
    close(inPipe[0]);

    if (!stdinData.empty()) {
        size_t off = 0;
        while (off < stdinData.size()) {
            ssize_t n = write(inPipe[1], stdinData.data() + off, stdinData.size() - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
    }
    close(inPipe[1]);

    const uint64_t deadline = getNowMs() + (uint64_t)timeoutMs;
    auto drain = [&](int fd, std::string& dst, size_t cap) {
        char tmp[8192];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof tmp)) > 0) {
            if (dst.size() < cap) {
                dst.append(tmp, (size_t)std::min<ssize_t>(n, (ssize_t)(cap - dst.size())));
            }
        }
    };

    for (;;) {
        struct pollfd fds[2];
        fds[0] = {outPipe[0], POLLIN, 0};
        fds[1] = {errPipe[0], POLLIN, 0};
        const uint64_t now = getNowMs();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        int rc = ::poll(fds, 2, (int)std::min<uint64_t>(deadline - now, 100));
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) drain(outPipe[0], out, kShellMaxOutput);
        if (fds[1].revents & (POLLIN | POLLHUP)) drain(errPipe[0], errOut, kShellMaxOutput);
        if (fds[0].revents == POLLHUP && fds[1].revents == POLLHUP) break;
        // Both pipes closed?
        if ((fds[0].revents & (POLLIN | POLLHUP)) == 0 && (fds[1].revents & (POLLIN | POLLHUP)) == 0 && rc == 0) {
            // poll timed out with nothing readable; loop again for deadline
        }
    }
    close(outPipe[0]);
    close(errPipe[0]);

    // Wait for the child (already signalled kill below on timeout)
    if (timedOut) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    const uint64_t waitDeadline = getNowMs() + 3000;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { status = 0; break; }
        if (getNowMs() > waitDeadline) { kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
        if (!g_running) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exitCode = 128 + WTERMSIG(status);
    else exitCode = -1;
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
    if (!runShellPosix(args, cwd, timeoutMs, stdinStr, exitCode, timedOut, out, errOut, error)) {
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
// Command Dispatch
// ============================================================================

static std::string cursorFields() {
    int x = 0, y = 0;
    getCursorPos(x, y);
    return "\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y);
}

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
        int x = 0, y = 0;
        getCursorPos(x, y);
        return okReply(req, "\"width\":" + std::to_string(g_screenW) + ",\"height\":" + std::to_string(g_screenH) +
                                ",\"cursor_x\":" + std::to_string(x) + ",\"cursor_y\":" + std::to_string(y));
    }

    if (cmd == "mouse_move") {
        float x = 0, y = 0;
        const int r = getPoint(req, "x", "y", x, y, err);
        if (r < 0) return errReply(req, err);
        if (r == 0) return errReply(req, "'x' and 'y' are required");
        setStatus("moving");
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

        setStatus("clicking");
        if (r == 1 && !moveHuman(x, y)) return errReply(req, "aborted");
        if (!sleepMs(randInt(60, 120))) return errReply(req, "aborted");
        for (int i = 0; i < count; i++) {
            buttonEvent(button, true);
            const bool up1 = sleepMs(randInt(35, 80));
            buttonEvent(button, false);
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

        setStatus("dragging");
        if (rf == 1 && !moveHuman(fx, fy)) return errReply(req, "aborted");
        if (!sleepMs(randInt(60, 110))) return errReply(req, "aborted");
        buttonEvent(Button::Left, true);
        bool ok = sleepMs(randInt(70, 120)) && moveHuman(x, y) && sleepMs(randInt(60, 110));
        buttonEvent(Button::Left, false);
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

        setStatus("scrolling");
        if (r == 1 && !moveHuman(x, y)) return errReply(req, "aborted");
        const int vy = (int)std::clamp(dyN, -50.0, 50.0);
        const int hx = (int)std::clamp(dxN, -50.0, 50.0);
        if (vy != 0) {
            sendWheel(vy > 0 ? 5 : 4, std::abs(vy));
        }
        if (hx != 0) {
            sendWheel(hx > 0 ? 7 : 6, std::abs(hx));
        }
        return okReply(req, cursorFields());
    }

    if (cmd == "keyboard_type") {
        std::string text;
        if (!getStr(req, "text", text)) return errReply(req, "'text' (string) is required");
        if (text.size() > 20000) return errReply(req, "'text' is too long (max 20000 bytes)");
        double delay = 12;
        getNum(req, "delay_ms", delay);
        setStatus("typing");
        if (!typeText(text, (int)std::clamp(delay, 0.0, 500.0))) return errReply(req, "aborted");
        return okReply(req);
    }

    if (cmd == "keyboard_key") {
        std::string keys;
        if (!getStr(req, "keys", keys)) return errReply(req, "'keys' (string, e.g. \"ctrl+c\" or \"ctrl+a ctrl+c\") is required");
        setStatus("pressing keys");
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
        setStatus("looking");
        std::string outPath;
        if (!captureScreen(p, outPath, err)) return errReply(req, err);
        return okReply(req, "\"path\":\"" + jsonEscape(outPath) + "\",\"width\":" + std::to_string(g_screenW) +
                                ",\"height\":" + std::to_string(g_screenH));
    }

    if (cmd == "wait") {
        double ms = 0;
        if (!getNum(req, "ms", ms)) return errReply(req, "'ms' (number) is required");
        setStatus("waiting");
        if (!sleepMs((int)std::clamp(ms, 0.0, 60000.0))) return errReply(req, "aborted");
        return okReply(req);
    }

    if (cmd == "fs_list" || cmd == "fs_read" || cmd == "fs_write" || cmd == "fs_mkdir" ||
        cmd == "fs_delete" || cmd == "fs_search" || cmd == "shell_exec") {
        setStatus("filesystem");
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

// Tool schemas in the format AI APIs expect (name / description / input_schema)
static const char* TOOL_DEFINITIONS = R"JSON([
  {"name":"screen_info","description":"Get the screen size in pixels and the current mouse position. All coordinates used by the other tools are in these pixels, origin at the top-left corner.","input_schema":{"type":"object","properties":{},"required":[]}},
  {"name":"screenshot","description":"Capture the whole screen to a PNG file and return its path, width and height.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Optional output path ending in .png. Defaults to a file in the temp folder."}},"required":[]}},
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
  {"name":"fs_search","description":"Search file names under a directory tree with a wildcard pattern (* and ?). Hidden files, dependency folders (.git, node_modules, ...) and syntax trees are skipped.","input_schema":{"type":"object","properties":{"pattern":{"type":"string","description":"Wildcard pattern, e.g. *.txt or report-*.md"},"path":{"type":"string","description":"Optional base directory, default the home root"},"max_results":{"type":"integer","description":"Optional cap, default 100, max 200"}},"required":["pattern"]}},
  {"name":"shell_exec","description":"Execute a program with an argv array (never a shell string) and return exit code, stdout and stderr. Only available when the user enabled shell tools in settings; requires a timeout (max 300000 ms) and returns at most 256 KiB of combined output. Optional 'stdin' feeds data to the process. Optional 'cwd' must be inside an allowed root.","input_schema":{"type":"object","properties":{"argv":{"type":"array","items":{"type":"string"},"description":"Program plus arguments, e.g. [\"ls\",\"-la\",\"reports\"]"},"cwd":{"type":"string","description":"Optional working directory (inside allowed roots)"},"timeout_ms":{"type":"integer","description":"Default 60000, max 300000"},"stdin":{"type":"string","description":"Optional data piped to the program's stdin"}},"required":["argv"]}},
  {"name":"end_session","description":"Finish: remove the overlay, restore the normal cursor and give mouse control back to the user. Call this when the task is done.","input_schema":{"type":"object","properties":{},"required":[]}}
])JSON";

static void printHelp() {
    std::printf(
        "matricX-cowork (Linux/X11) - computer-control tool for the M-Cowork AI client\n\n"
        "Usage: matricX-cowork [options]\n\n"
        "Protocol: one JSON object per line on stdin, one JSON reply per line on stdout.\n"
        "  -> {\"id\":1,\"cmd\":\"mouse_move\",\"x\":500,\"y\":300}\n"
        "  <- {\"id\":1,\"ok\":true,\"x\":500,\"y\":300}\n"
        "Events: {\"event\":\"ready\",...} on start, {\"event\":\"stopped\",\"reason\":...} on exit.\n"
        "Run with --describe to list every command and its parameters.\n\n"
        "Options:\n"
        "  --speed <mode>        slow | normal | fast (default normal)\n"
        "  --glow <theme>        green | cyan | rainbow | off (default green)\n"
        "  --idle-timeout <sec>  End the session after this long without commands (default 120, 0 = never)\n"
        "  --no-cursor           Skip custom cursor replacement\n"
        "  --no-lock             Don't block manual mouse movement\n"
        "  --enable-shell        Allow the shell_exec tool (off by default)\n"
        "  --allow-root <path>   Extend the fs/shell allowed roots (default: $HOME and temp)\n"
        "  --describe            Print the tool definitions as JSON and exit\n"
        "  --restore-cursor      Restore default system cursors and exit\n"
        "  --help                Show this help\n"
        "  --version             Show version\n\n"
    );
}

static bool parseArgs(int argc, char** argv, int& exitCode) {
    exitCode = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return false;
        }
        if (arg == "--version") {
            std::printf("%s\n", VERSION_STRING);
            return false;
        }
        if (arg == "--describe") {
            std::fputs(TOOL_DEFINITIONS, stdout);
            std::fputc('\n', stdout);
            return false;
        }
        if (arg == "--restore-cursor") {
            std::printf("System cursors restored.\n");
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
        std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        printHelp();
        exitCode = 2;
        return false;
    }
    return true;
}

static void sigHandler(int sig) {
    (void)sig;
    requestStop(StopConsole);
}

int main(int argc, char** argv) {
    int exitCode = 0;
    if (!parseArgs(argc, argv, exitCode)) return exitCode;

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGPIPE, SIG_IGN);  // child pipes closing early must not kill the engine

    XSetErrorHandler(x11ErrorHandler);

    g_rng.seed((unsigned)std::time(nullptr) ^ (unsigned)getpid());
    g_startTime = std::chrono::steady_clock::now();
    g_lastActivityMs = getNowMs();

    // Connect to X11 if available
    g_display = XOpenDisplay(nullptr);
    if (g_display) {
        g_screen = DefaultScreen(g_display);
        g_root = RootWindow(g_display, g_screen);
        g_screenW = DisplayWidth(g_display, g_screen);
        g_screenH = DisplayHeight(g_display, g_screen);
    } else {
        std::fprintf(stderr, "Notice: No X11 display found (running headless mode)\n");
    }

    emitLine(std::string("{\"event\":\"ready\",\"version\":\"") + VERSION_STRING + "\",\"width\":" +
             std::to_string(g_screenW) + ",\"height\":" + std::to_string(g_screenH) + "}");

    // Idle watchdog thread
    std::thread watchdog([&] {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (g_idleTimeoutSec > 0) {
                uint64_t idle = getNowMs() - g_lastActivityMs.load();
                if (idle > (uint64_t)g_idleTimeoutSec * 1000ULL) {
                    requestStop(StopIdle);
                    break;
                }
            }
        }
    });

    // Stdin command reading loop
    std::string line;
    while (g_running) {
        line.clear();
        int c = 0;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
            line.push_back((char)c);
        }
        if (c == EOF && line.empty()) break;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
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

        g_lastActivityMs = getNowMs();
        std::string cmd;
        getStr(req, "cmd", cmd);
        bool endSession = false;
        const std::string reply = dispatch(req, cmd, endSession);
        g_lastActivityMs = getNowMs();
        setStatus("ready");
        emitLine(reply);

        if (endSession) {
            requestStop(StopEndSession);
            break;
        }
    }

    requestStop(StopInputClosed);
    g_running = false;
    if (watchdog.joinable()) watchdog.join();

    if (g_display) {
        XCloseDisplay(g_display);
        g_display = nullptr;
    }

    emitLine(std::string("{\"event\":\"stopped\",\"reason\":\"") + stopReasonName(g_stopReason.load()) + "\"}");
    return 0;
}
