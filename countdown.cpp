// ============================================================================
//  countdown.cpp —— C++ 倒计时器 (Windows)
// ----------------------------------------------------------------------------
//  功能：
//    * 自定义 天 / 小时 / 分钟 / 秒
//    * 大字实时显示 + 进度条 + 预计结束时间
//    * 空格 = 暂停/继续，ESC = 提前退出
//    * 结束时：自定义声音提示 + 弹窗提示 + 任务栏闪烁
//    * 声音、弹窗标题/内容可通过 timer.ini 或命令行参数自定义
//
//  编译（VS 开发者命令提示符）：
//    cl /utf-8 /EHsc /O2 /std:c++17 countdown.cpp /link winmm.lib
//  或直接双击 build.bat
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>

#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <ctime>

#pragma comment(lib, "winmm.lib")

using std::string;
using std::wstring;
using std::vector;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
//  全局控制台句柄
// ---------------------------------------------------------------------------
static HANDLE g_hOut = nullptr;
static HANDLE g_hIn  = nullptr;
static bool   g_tty  = false;   // 标准输出是否为真实控制台（被重定向时为 false）
static bool   g_conIn = false;  // 标准输入是否为真实控制台（被重定向时为 false）

// 颜色
enum : WORD {
    C_DEFAULT = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    C_TITLE   = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,          // 亮青
    C_DIGIT   = FOREGROUND_GREEN | FOREGROUND_INTENSITY,                            // 亮绿
    C_DIM     = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_WARN    = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,           // 黄
    C_ALARM   = FOREGROUND_RED | FOREGROUND_INTENSITY,                              // 亮红
    C_LABEL   = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,          // 亮青
};

static void setColor(WORD c)  { if (g_tty) SetConsoleTextAttribute(g_hOut, c); }
static void resetColor()      { setColor(C_DEFAULT); }
static void gotoxy(int x, int y) {
    if (!g_tty) return;
    COORD p{ (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(g_hOut, p);
}
static void hideCursor() { if (g_tty) { CONSOLE_CURSOR_INFO ci{ 100, FALSE }; SetConsoleCursorInfo(g_hOut, &ci); } }
static void showCursor() { if (g_tty) { CONSOLE_CURSOR_INFO ci{ 100, TRUE  }; SetConsoleCursorInfo(g_hOut, &ci); } }

// ---------------------------------------------------------------------------
//  UTF-8 <-> UTF-16 转换
// ---------------------------------------------------------------------------
static wstring widen(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static string narrow(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 计算 UTF-8 字符串在终端中的显示宽度（中日韩字符算 2 列）
static int dispWidth(const string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned int cp = 0;
        int len = 1;
        if (c < 0x80)              { cp = c;        len = 1; }
        else if ((c >> 5) == 0x06) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0x0E) { cp = c & 0x0F; len = 3; }
        else                       { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += (size_t)len;

        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F9FF);
        w += wide ? 2 : 1;
    }
    return w;
}

// 输出一行并把剩余宽度补空格（用于原地重绘，避免旧内容残留）
static void printPadded(const string& s, int width) {
    fputs(s.c_str(), stdout);
    int w = dispWidth(s);
    for (int i = w; i < width; ++i) fputc(' ', stdout);
    fputc('\n', stdout);
}

// 居中输出一行并补齐宽度
static void printCentered(const string& s, int width) {
    int pad = (width - dispWidth(s)) / 2;
    if (pad < 0) pad = 0;
    printPadded(string((size_t)pad, ' ') + s, width);
}

// ---------------------------------------------------------------------------
//  配置（timer.ini）
// ---------------------------------------------------------------------------
struct Config {
    wstring sound   = L"alarm.wav";      // 声音文件；空串表示用系统提示音
    bool    loop    = true;              // 是否循环播放
    wstring title   = L"倒计时结束";      // 弹窗标题
    wstring message = L"时间到！\n\n倒计时 {time} 已结束。";  // 弹窗内容，{time} 会被替换
    bool    popup   = true;              // 是否弹出窗口
    bool    flash   = true;              // 是否闪烁任务栏
    bool    beep    = false;             // 是否额外播放系统提示音
};

static wstring trimW(const wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static wstring toLowerW(wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool endsWithW(const wstring& s, const wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

static wstring exeDir() {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == wstring::npos) ? L"." : p.substr(0, pos);
}

static bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 读取 UTF-8 文本文件为宽字符串
static bool readTextUtf8(const wstring& path, wstring& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    string bytes = ss.str();
    // 去掉 UTF-8 BOM
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes = bytes.substr(3);
    out = widen(bytes);
    return true;
}

static Config loadConfig() {
    Config cfg;
    vector<wstring> candidates = { exeDir() + L"\\timer.ini", L"timer.ini" };

    wstring text;
    bool loaded = false;
    for (auto& f : candidates) {
        if (fileExists(f) && readTextUtf8(f, text)) { loaded = true; break; }
    }
    if (!loaded) return cfg;

    std::wistringstream in(text);
    wstring line;
    while (std::getline(in, line)) {
        line = trimW(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[') continue;
        size_t eq = line.find(L'=');
        if (eq == wstring::npos) continue;

        wstring key = toLowerW(trimW(line.substr(0, eq)));
        wstring val = trimW(line.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == L'"' && val.back() == L'"') ||
                                (val.front() == L'\'' && val.back() == L'\'')))
            val = val.substr(1, val.size() - 2);

        auto truthy = [](const wstring& v) {
            wstring t = toLowerW(trimW(v));
            return t == L"1" || t == L"true" || t == L"yes" || t == L"on" || t == L"是";
        };

        if      (key == L"sound")   cfg.sound   = val;
        else if (key == L"loop")    cfg.loop    = truthy(val);
        else if (key == L"title")   cfg.title   = val;
        else if (key == L"message") cfg.message = val;
        else if (key == L"popup")   cfg.popup   = truthy(val);
        else if (key == L"flash")   cfg.flash   = truthy(val);
        else if (key == L"beep")    cfg.beep    = truthy(val);
    }

    // 支持 message 中用 \n 表示换行
    wstring msg;
    for (size_t i = 0; i < cfg.message.size(); ++i) {
        if (cfg.message[i] == L'\\' && i + 1 < cfg.message.size() && cfg.message[i + 1] == L'n') {
            msg += L'\n'; ++i;
        } else msg += cfg.message[i];
    }
    cfg.message = msg;
    return cfg;
}

// ---------------------------------------------------------------------------
//  声音播放器：wav 用 PlaySound，mp3 等用 MCI，也支持系统声音别名
// ---------------------------------------------------------------------------
class SoundPlayer {
public:
    ~SoundPlayer() { stop(); }

    void play(wstring src, bool loop) {
        stop();
        if (src.empty()) {   // 空 => 系统提示音
            PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
            m_playing = true;
            return;
        }
        src = resolve(src);

        // 1) 系统声音别名，例如 SystemExclamation / SystemHand / SystemAsterisk
        if (!fileExists(src) && src.find(L'.') == wstring::npos) {
            PlaySoundW(src.c_str(), nullptr, SND_ALIAS | SND_ASYNC | (loop ? SND_LOOP : 0));
            m_playing = true;
            return;
        }
        // 2) .wav 文件
        if (endsWithW(toLowerW(src), L".wav")) {
            DWORD flags = SND_FILENAME | SND_ASYNC | SND_NODEFAULT | (loop ? SND_LOOP : 0);
            if (PlaySoundW(src.c_str(), nullptr, flags)) { m_playing = true; return; }
        }
        // 3) 其它格式（mp3 / mid / flac ...）交给 MCI
        if (mciPlay(src, loop)) { m_playing = true; return; }
        // 4) 兜底
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
        m_playing = true;
    }

    void stop() {
        if (!m_playing) return;
        PlaySoundW(nullptr, nullptr, 0);
        mciSendStringW(L"close cdtimer_snd", nullptr, 0, nullptr);
        m_playing = false;
    }

    // 相对路径：先找当前目录，再找 exe 所在目录
    static wstring resolve(const wstring& p) {
        if (fileExists(p)) return p;
        wstring alt = exeDir() + L"\\" + p;
        if (fileExists(alt)) return alt;
        return p;
    }

private:
    bool m_playing = false;

    bool mciPlay(const wstring& file, bool loop) {
        mciSendStringW(L"close cdtimer_snd", nullptr, 0, nullptr);
        wstring cmd = L"open \"" + file + L"\" alias cdtimer_snd";
        if (mciSendStringW(cmd.c_str(), nullptr, 0, nullptr) != 0) return false;
        if (mciSendStringW(loop ? L"play cdtimer_snd repeat" : L"play cdtimer_snd",
                           nullptr, 0, nullptr) != 0) {
            mciSendStringW(L"play cdtimer_snd", nullptr, 0, nullptr);  // 不支持 repeat 时单次播放
        }
        return true;
    }
};

static SoundPlayer g_sound;

// Ctrl+C 等退出时停止声音
static BOOL WINAPI onCtrl(DWORD type) {
    g_sound.stop();
    PlaySoundW(nullptr, nullptr, 0);
    return FALSE;
}

// ---------------------------------------------------------------------------
//  时间格式化
// ---------------------------------------------------------------------------
static string fmtHMS(long long sec) {
    if (sec < 0) sec = 0;
    long long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    char buf[48];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", h, m, s);
    return buf;
}

// 人类可读："1 天 02:03:04"
static wstring fmtHuman(long long sec) {
    long long d = sec / 86400;
    long long rest = sec % 86400;
    string hms = fmtHMS(rest);
    std::wostringstream w;
    if (d > 0) w << d << L" 天 ";
    w << widen(hms);
    return w.str();
}

// 剩余时间的大字文本（小时可超过 99）
static string hmsBig(long long sec) {
    return fmtHMS(sec);
}

static string fmtEndTime(long long totalSec) {
    time_t now = time(nullptr);
    time_t end = now + (time_t)totalSec;
    struct tm tmv{};
    localtime_s(&tmv, &end);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// ---------------------------------------------------------------------------
//  5 行高的数字字形
// ---------------------------------------------------------------------------
static const char* kGlyph[10][5] = {
    { "████", "█  █", "█  █", "█  █", "████" }, // 0
    { "  █ ", " ██ ", "  █ ", "  █ ", " ███" }, // 1
    { "████", "   █", "████", "█   ", "████" }, // 2
    { "████", "   █", "████", "   █", "████" }, // 3
    { "█  █", "█  █", "████", "   █", "   █" }, // 4
    { "████", "█   ", "████", "   █", "████" }, // 5
    { "████", "█   ", "████", "█  █", "████" }, // 6
    { "████", "   █", "   █", "   █", "   █" }, // 7
    { "████", "█  █", "████", "█  █", "████" }, // 8
    { "████", "█  █", "████", "   █", "████" }, // 9
};
static const char* kColon[5] = { "  ", "██", "  ", "██", "  " };

static string bigRow(const string& text, int row) {
    string out;
    for (char ch : text) {
        if (ch == ':') out += kColon[row];
        else if (ch >= '0' && ch <= '9') out += kGlyph[ch - '0'][row];
        else out += "    ";
        out += " ";
    }
    if (!out.empty()) out.pop_back();   // 去掉末尾多余间隔
    return out;
}

// 进度条
static string progressBar(double ratio, int width) {
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int filled = (int)std::lround(ratio * width);
    string s = "[";
    for (int i = 0; i < width; ++i) s += (i < filled ? "█" : "░");
    s += "]";
    return s;
}

// ---------------------------------------------------------------------------
//  键盘检测（非阻塞，只响应真实按键，程序在后台时不会误触发）
// ---------------------------------------------------------------------------
enum class Key { None, Space, Esc, Enter, Other };

static Key pollKey() {
    if (!g_conIn) return Key::None;      // 输入不是控制台时不做键盘检测
    INPUT_RECORD rec;
    DWORD n = 0;
    for (;;) {
        if (!PeekConsoleInputW(g_hIn, &rec, 1, &n) || n == 0) return Key::None;
        ReadConsoleInputW(g_hIn, &rec, 1, &n);
        if (rec.EventType != KEY_EVENT) continue;
        if (!rec.Event.KeyEvent.bKeyDown) continue;
        switch (rec.Event.KeyEvent.wVirtualKeyCode) {
            case VK_SPACE:  return Key::Space;
            case VK_ESCAPE: return Key::Esc;
            case VK_RETURN: return Key::Enter;
            default:        return Key::Other;
        }
    }
}

static void clearInput() { if (g_conIn) FlushConsoleInputBuffer(g_hIn); }

// 读取一行输入（UTF-8 字节流），失败返回 false
static bool readLine(string& out) {
    char buf[1024];
    out.clear();
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return true;
}

// 等待一次按键（用于结束后的交互）
static void waitAnyKey(const char* hint) {
    clearInput();
    printf("%s", hint);
    fflush(stdout);
    for (;;) {
        if (pollKey() != Key::None) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// ---------------------------------------------------------------------------
//  解析时长输入
//    支持: "0 1 30 0"（天 时 分 秒） / "01:30:00" / "5400"（秒）
// ---------------------------------------------------------------------------
static bool parseDuration(const string& raw, long long& outSec) {
    string s;
    for (char c : raw) {
        if (c == ',') s += ' ';
        else s += c;
    }

    // 冒号格式
    if (s.find(':') != string::npos) {
        long long parts[3] = { 0, 0, 0 };
        int cnt = 0;
        std::stringstream ss(s);
        string tok;
        while (std::getline(ss, tok, ':')) {
            if (cnt >= 3) return false;
            tok = trimW(widen(tok)).empty() ? "0" : tok;
            char* end = nullptr;
            long long v = strtoll(tok.c_str(), &end, 10);
            if (end == tok.c_str() || v < 0) return false;
            parts[cnt++] = v;
        }
        if (cnt == 2)      outSec = parts[0] * 60 + parts[1];
        else if (cnt == 3) outSec = parts[0] * 3600 + parts[1] * 60 + parts[2];
        else return false;
        return true;
    }

    // 空格分隔 1~4 个数字，依次对应 天 时 分 秒
    std::istringstream iss(s);
    vector<long long> nums;
    string tok;
    while (iss >> tok) {
        char* end = nullptr;
        long long v = strtoll(tok.c_str(), &end, 10);
        if (end == tok.c_str() || v < 0) return false;
        nums.push_back(v);
        if (nums.size() > 4) return false;
    }
    if (nums.empty()) return false;

    if (nums.size() == 1) { outSec = nums[0]; return true; }
    long long mult[4] = { 86400, 3600, 60, 1 };
    long long total = 0;
    size_t offset = 4 - nums.size();          // 少输入时按"从秒开始"对齐
    for (size_t i = 0; i < nums.size(); ++i)
        total += nums[i] * mult[offset + i];
    outSec = total;
    return true;
}

// ---------------------------------------------------------------------------
//  界面绘制
// ---------------------------------------------------------------------------
static const int UI_WIDTH   = 72;   // 每行补齐的显示宽度
static const int DYN_TOP    = 6;    // 动态区域起始行
static const int DYN_HEIGHT = 9;    // 动态区域行数

static void drawHeader(const Config& cfg, long long totalSec) {
    setColor(C_TITLE);
    printf("%s\n", string(UI_WIDTH, '=').c_str());
    resetColor();

    // 标题居中
    setColor(C_DIGIT);
    printCentered("C++  倒 计 时 器", UI_WIDTH);
    resetColor();

    setColor(C_TITLE);
    printf("%s\n", string(UI_WIDTH, '=').c_str());
    resetColor();

    // 信息行
    string sndName;
    if (cfg.sound.empty()) sndName = "系统提示音";
    else sndName = narrow(cfg.sound) + (cfg.loop ? " [循环]" : " [单次]");

    string info = "  时长: " + narrow(fmtHuman(totalSec)) + "     预计结束: " + fmtEndTime(totalSec);
    printPadded(info, UI_WIDTH);

    string info2 = "  提示音: " + sndName + (cfg.popup ? "     弹窗: 开" : "     弹窗: 关");
    printPadded(info2, UI_WIDTH);

    printf("\n");
}

static void drawDynamic(long long remainMs, long long totalMs, bool paused, bool finished) {
    long long remainSec = (remainMs + 999) / 1000;   // 向上取整，避免显示 00:00:00 却还没结束
    if (remainSec < 0) remainSec = 0;
    if (finished) remainSec = 0;

    string hms = hmsBig(remainSec);

    gotoxy(0, DYN_TOP);

    // 5 行大数字
    setColor(finished ? C_ALARM : (paused ? C_WARN : C_DIGIT));
    for (int r = 0; r < 5; ++r) printCentered(bigRow(hms, r), UI_WIDTH);
    resetColor();

    // 空行
    printPadded("", UI_WIDTH);

    // 进度条 + 百分比（宽度固定，避免抖动）
    double ratio = totalMs > 0 ? (double)(totalMs - remainMs) / (double)totalMs : 1.0;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int pct = (int)std::lround(ratio * 100);

    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%3d%%", pct);
    setColor(paused ? C_WARN : C_LABEL);
    printCentered(progressBar(ratio, 40) + "  " + pctBuf, UI_WIDTH);
    resetColor();

    // 空行
    printPadded("", UI_WIDTH);

    // 状态行
    string state;
    WORD   color;
    if (finished)   { state = "   [ 时间到 ]"; color = C_ALARM; }
    else if (paused) { state = "   [ 已暂停 ]  按空格继续"; color = C_WARN; }
    else             { state = "   [ 运行中 ]"; color = C_DIGIT; }
    setColor(color);
    printPadded(state, UI_WIDTH);
    resetColor();

    fflush(stdout);
}

static void drawFooter() {
    gotoxy(0, DYN_TOP + DYN_HEIGHT);       // 定位到动态区域下方，避免被覆盖
    setColor(C_DIM);
    printf("%s\n", string(UI_WIDTH, '-').c_str());
    printPadded("  空格 = 暂停/继续      ESC = 退出", UI_WIDTH);
    resetColor();
    fflush(stdout);
}

// 保证控制台窗口足够大，避免界面被裁切（只在窗口太小时放大，不缩小）
static void ensureConsoleSize(int cols, int rows) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_hOut, &info)) return;

    int winW = info.srWindow.Right - info.srWindow.Left + 1;
    int winH = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (winW >= cols && winH >= rows) return;

    int newW = std::max(winW, cols);
    int newH = std::max(winH, rows);

    // 缓冲区必须先足够大，窗口才能放大
    COORD buf{ (SHORT)std::max((int)info.dwSize.X, newW),
               (SHORT)std::max((int)info.dwSize.Y, newH) };
    if (buf.X != info.dwSize.X || buf.Y != info.dwSize.Y) {
        if (!SetConsoleScreenBufferSize(g_hOut, buf)) return;
    }

    SMALL_RECT r{ 0, 0, (SHORT)(newW - 1), (SHORT)(newH - 1) };
    SetConsoleWindowInfo(g_hOut, TRUE, &r);
}

// ---------------------------------------------------------------------------
//  结束提示：闪烁 + 声音 + 弹窗
// ---------------------------------------------------------------------------
static void flashWindow() {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = hwnd;
    fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    fi.uCount = 0;         // 0 = 持续闪烁，直到窗口被激活
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);
}

static void stopFlash() {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = hwnd;
    fi.dwFlags = FLASHW_STOP;
    FlashWindowEx(&fi);
}

static void showFinishPopup(const Config& cfg, long long totalSec) {
    // 替换 {time} 占位符
    wstring msg = cfg.message;
    wstring ph = L"{time}";
    for (size_t p = msg.find(ph); p != wstring::npos; p = msg.find(ph, p)) {
        msg.replace(p, ph.size(), fmtHuman(totalSec));
    }
    wstring title = cfg.title.empty() ? L"倒计时结束" : cfg.title;

    MessageBoxW(nullptr, msg.c_str(), title.c_str(),
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------------------
//  一次完整的倒计时
// ---------------------------------------------------------------------------
static void runCountdown(const Config& cfg, long long totalSec) {
    long long totalMs = totalSec * 1000;

    if (g_tty) {
        system("cls");
        drawHeader(cfg, totalSec);
        drawFooter();
    } else {
        printf("倒计时开始: %s\n", narrow(fmtHuman(totalSec)).c_str());
    }

    // 记录控制台标题
    wstring oldTitle;
    {
        wchar_t buf[512] = { 0 };
        DWORD n = GetConsoleTitleW(buf, 512);
        oldTitle.assign(buf, n);
    }

    clearInput();
    hideCursor();

    long long elapsedMs = 0;
    bool paused = false;
    bool quit   = false;
    auto last = Clock::now();

    for (;;) {
        auto now = Clock::now();
        long long dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        last = now;
        if (dt < 0) dt = 0;
        if (dt > 1000) dt = 1000;          // 防止系统休眠唤醒后一次性跳过

        Key k = pollKey();
        if (k == Key::Esc) { quit = true; break; }
        if (k == Key::Space) {
            paused = !paused;
        }

        if (!paused) elapsedMs += dt;

        long long remain = totalMs - elapsedMs;
        if (remain < 0) remain = 0;

        if (g_tty) {
            drawDynamic(remain, totalMs, paused, remain == 0);
            // 控制台标题同步显示剩余时间
            wstring t = L"倒计时 " + fmtHuman((remain + 999) / 1000);
            SetConsoleTitleW(t.c_str());
        }

        if (remain == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SetConsoleTitleW(oldTitle.empty() ? L"命令提示符" : oldTitle.c_str());
    showCursor();

    if (quit) {
        if (g_tty) {
            gotoxy(0, DYN_TOP + DYN_HEIGHT + 2);
            setColor(C_WARN);
            printf("已提前退出倒计时。\n");
            resetColor();
        }
        fflush(stdout);
        return;
    }

    // ---------------- 时间到 ----------------
    if (g_tty) {
        // 覆盖底部的操作提示区，显示结束横幅
        gotoxy(0, DYN_TOP + DYN_HEIGHT);
        setColor(C_ALARM);
        printf("%s\n", string(UI_WIDTH, '=').c_str());
        printCentered("时  间  到  ！", UI_WIDTH);
        printf("%s\n", string(UI_WIDTH, '=').c_str());
        resetColor();
        fflush(stdout);
    } else {
        printf("时间到！(%s)\n", narrow(fmtHuman(totalSec)).c_str());
    }

    if (cfg.flash) flashWindow();

    // 播放声音
    g_sound.play(cfg.sound, cfg.loop);
    if (cfg.beep) MessageBeep(MB_ICONEXCLAMATION);

    if (cfg.popup) {
        showFinishPopup(cfg, totalSec);
    } else if (g_tty) {
        // 无弹窗时，控制台一直提示到用户按键
        waitAnyKey("  按任意键停止提示音...");
    }

    g_sound.stop();
    if (cfg.flash) stopFlash();

    // 控制台里补一条完成记录
    if (g_tty) {
        gotoxy(0, DYN_TOP + DYN_HEIGHT + 4);
        setColor(C_DIM);
        printf("  倒计时结束于 ");
        SYSTEMTIME st; GetLocalTime(&st);
        printf("%04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        resetColor();
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
//  交互式输入时长
// ---------------------------------------------------------------------------
static bool askDuration(long long& sec) {
    if (g_tty) system("cls");
    setColor(C_TITLE);
    printf("%s\n", string(UI_WIDTH, '=').c_str());
    resetColor();
    setColor(C_DIGIT);
    string t = "C++  倒 计 时 器";
    printf("%s%s\n", string((size_t)((UI_WIDTH - dispWidth(t)) / 2), ' ').c_str(), t.c_str());
    resetColor();
    setColor(C_TITLE);
    printf("%s\n\n", string(UI_WIDTH, '=').c_str());
    resetColor();

    printf("  请输入倒计时时长，支持以下格式：\n\n");
    printf("    天 时 分 秒    例如  0 1 30 0   （0天1小时30分0秒）\n");
    printf("    时:分:秒       例如  01:30:00\n");
    printf("    纯秒数         例如  5400\n\n");
    printf("  > ");
    fflush(stdout);

    string line;
    if (!readLine(line)) return false;

    if (!parseDuration(line, sec)) {
        setColor(C_ALARM);
        printf("\n  输入格式不正确，请重新运行程序。\n");
        resetColor();
        return false;
    }
    if (sec <= 0) {
        setColor(C_ALARM);
        printf("\n  时长必须大于 0 秒。\n");
        resetColor();
        return false;
    }
    return true;
}

static bool askRestart() {
    if (!g_tty) return false;
    printf("\n  是否重新设置一个倒计时？(y/N) ");
    fflush(stdout);
    string line;
    if (!readLine(line)) return false;
    for (auto& c : line) c = (char)tolower((unsigned char)c);
    return !line.empty() && (line[0] == 'y' || line[0] == '1');
}

// ---------------------------------------------------------------------------
//  命令行帮助
// ---------------------------------------------------------------------------
static void printHelp() {
    printf("C++ 倒计时器 v1.0\n\n");
    printf("用法:\n");
    printf("  countdown.exe                        交互式输入时长\n");
    printf("  countdown.exe 0 1 30 0               天 时 分 秒\n");
    printf("  countdown.exe 01:30:00               时:分:秒\n");
    printf("  countdown.exe 90                     90 秒\n\n");
    printf("选项:\n");
    printf("  -s, --sound <文件>     指定提示音（.wav 或 .mp3）\n");
    printf("  -t, --title <文字>     弹窗标题\n");
    printf("  -m, --message <文字>   弹窗内容，{time} 会替换为总时长\n");
    printf("  -n, --no-loop          提示音只播放一次\n");
    printf("  -q, --no-popup         不弹窗（仅控制台提示）\n");
    printf("      --no-flash         不闪烁任务栏\n");
    printf("  -h, --help             显示本帮助\n\n");
    printf("运行时:\n");
    printf("  空格 = 暂停/继续      ESC = 退出\n\n");
    printf("提示: 与 exe 同目录的 timer.ini 可以长期保存这些设置。\n");
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[]) {
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);

    DWORD mode = 0;
    g_tty   = GetConsoleMode(g_hOut, &mode) != 0;
    g_conIn = GetConsoleMode(g_hIn, &mode) != 0;

    if (g_tty) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        SetConsoleTitleW(L"C++ 倒计时器");
        SetConsoleCtrlHandler(onCtrl, TRUE);
        ensureConsoleSize(UI_WIDTH + 8, 23);
    }

    Config cfg = loadConfig();

    // 调试用：输出一帧界面快照后退出（不影响正常使用）
    if (getenv("COUNTDOWN_UI_TEST")) {
        long long total = 3661;                  // 1 小时 1 分 1 秒
        long long remainMs = 1800LL * 1000;      // 剩余 30 分钟
        drawHeader(cfg, total);
        drawDynamic(remainMs, total * 1000LL, false, false);
        drawFooter();
        return 0;
    }

    long long totalSec = 0;
    bool hasTime = false;
    bool interactive = (argc <= 1);

    for (int i = 1; i < argc; ++i) {
        wstring a = argv[i];
        auto next = [&](wstring& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };

        if (a == L"-h" || a == L"--help" || a == L"/?") {
            printHelp();
            return 0;
        } else if (a == L"-s" || a == L"--sound") {
            wstring v; if (!next(v)) { printf("缺少参数: --sound\n"); return 1; }
            cfg.sound = v;
        } else if (a == L"-t" || a == L"--title") {
            wstring v; if (!next(v)) { printf("缺少参数: --title\n"); return 1; }
            cfg.title = v;
        } else if (a == L"-m" || a == L"--message") {
            wstring v; if (!next(v)) { printf("缺少参数: --message\n"); return 1; }
            cfg.message = v;
        } else if (a == L"-n" || a == L"--no-loop") {
            cfg.loop = false;
        } else if (a == L"-q" || a == L"--no-popup") {
            cfg.popup = false;
        } else if (a == L"--no-flash") {
            cfg.flash = false;
        } else if (a == L"--no-sound") {
            cfg.sound = L"";
        } else {
            // 剩余参数当作时间
            string joined = narrow(a);
            for (int j = i + 1; j < argc; ++j) {
                wstring nxt = argv[j];
                if (!nxt.empty() && (nxt[0] == L'-' || nxt[0] == L'/')) break;
                joined += " ";
                joined += narrow(nxt);
                ++i;
            }
            long long v = 0;
            if (!parseDuration(joined, v)) {
                printf("无法解析的时间: %s\n", joined.c_str());
                printHelp();
                return 1;
            }
            totalSec += v;
            hasTime = true;
        }
    }

    if (!hasTime && !interactive) {
        // 只给了选项没给时间，也进入交互输入
        interactive = true;
    }

    for (;;) {
        long long sec = totalSec;
        if (interactive) {
            if (!askDuration(sec)) {
                printf("\n  按回车键退出...");
                fflush(stdout);
                char dummy[8];
                fgets(dummy, sizeof(dummy), stdin);
                return 1;
            }
        }
        if (sec <= 0) {
            printf("时长必须大于 0 秒。\n");
            return 1;
        }

        runCountdown(cfg, sec);

        if (!interactive) break;      // 命令行模式跑完就结束
        if (!askRestart()) break;
    }

    if (interactive && g_tty) {
        // 交互模式下停留，避免窗口一闪而过
        printf("\n  按任意键关闭窗口...");
        fflush(stdout);
        clearInput();
        while (pollKey() == Key::None)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    resetColor();
    return 0;
}
