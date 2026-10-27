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
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

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

static const char* VERSION_STRING = "matricX-cowork 0.4.0 (Linux/X11)";

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
        const size_t n = std::strlen(lit);
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
            const double d = std::strtod(m_s.c_str() + m_i, &end);
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


    if (cmd == "fs_list") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' (directory path) is required");
        std::error_code ec;
        const std::filesystem::path dir(rawPath);
        if (!std::filesystem::is_directory(dir, ec) || ec) return errReply(req, "path is not a readable directory");
        std::string entries = "\\\"entries\\\":[";
        size_t count = 0;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && count < 200; it.increment(ec)) {
            if (count++) entries += ",";
            const auto name = it->path().filename().u8string();
            const bool isDir = it->is_directory(ec);
            const auto size = isDir ? 0ull : it->file_size(ec);
            if (ec) { ec.clear(); }
            entries += "{\\\"name\\\":\\\"" + jsonEscape(name) + "\\\",\\\"directory\\\":" + (isDir ? "true" : "false") +
                       ",\\\"size\\\":" + std::to_string(size) + "}";
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
        return okReply(req, "\\\"path\\\":\\\"" + jsonEscape(file.u8string()) + "\\\",\\\"content\\\":\\\"" + jsonEscape(content) +
                            "\\\",\\\"bytes\\\":" + std::to_string(content.size()));
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
        return okReply(req, "\\\"path\\\":\\\"" + jsonEscape(file.u8string()) + "\\\",\\\"bytes_written\\\":" + std::to_string(content.size()));
    }

    if (cmd == "fs_mkdir") {
        std::string rawPath;
        if (!getStr(req, "path", rawPath)) return errReply(req, "'path' (directory path) is required");
        std::error_code ec;
        const bool created = std::filesystem::create_directories(std::filesystem::path(rawPath), ec);
        if (ec) return errReply(req, "cannot create directory: " + ec.message());
        return okReply(req, "\\\"path\\\":\\\"" + jsonEscape(rawPath) + "\\\",\\\"created\\\":" + (created ? "true" : "false"));
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
        return okReply(req, "\\\"path\\\":\\\"" + jsonEscape(target.u8string()) + "\\\",\\\"deleted\\\":true");
    }

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
  {"name":"fs_list","description":"List up to 200 entries in a directory. Returns names, directory flags, and file sizes.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path to list"}},"required":["path"]}},
  {"name":"fs_read","description":"Read a UTF-8/text file up to 1 MiB. Returns its content and byte count.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path to read"}},"required":["path"]}},
  {"name":"fs_write","description":"Create a text file up to 1 MiB. Existing files are preserved unless overwrite=true is explicitly supplied.","input_schema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"},"overwrite":{"type":"boolean","description":"Must be true to replace an existing file"}},"required":["path","content"]}},
  {"name":"fs_mkdir","description":"Create a directory and any missing parent directories.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"Directory path to create"}},"required":["path"]}},
  {"name":"fs_delete","description":"Delete one file. Directory deletion and root-path deletion are refused.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"File path to delete"}},"required":["path"]}},
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
