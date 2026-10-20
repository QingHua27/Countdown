// ============================================================================
//  countdown_gui.cpp —— C++ 倒计时器（图形界面版 / Win32）
// ----------------------------------------------------------------------------
//  功能：
//    * 图形窗口，天 / 小时 / 分钟 / 秒 四个输入框
//    * 大字剩余时间 + 进度条 + 百分比
//    * 开始 / 暂停 / 重置 按钮，空格快捷键
//    * 结束时：自定义声音（wav/mp3/系统音）+ 弹窗提示 + 任务栏闪烁
//    * 与 exe 同目录的 timer.ini 可配置声音、弹窗文字等
//
//  编译（VS 开发者命令提示符）：
//    cl /utf-8 /EHsc /O2 /std:c++17 countdown_gui.cpp /link user32.lib gdi32.lib
//         winmm.lib comdlg32.lib /SUBSYSTEM:WINDOWS
//  或直接双击 build_gui.bat
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <string>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cmath>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using std::wstring;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
//  控件 ID
// ---------------------------------------------------------------------------
#define ID_EDIT_DAY     101
#define ID_EDIT_HOUR    102
#define ID_EDIT_MIN     103
#define ID_EDIT_SEC     104
#define ID_BTN_START    110
#define ID_BTN_PAUSE    111
#define ID_BTN_RESET    112
#define ID_BTN_SOUND    113
#define ID_CHK_POPUP    114
#define ID_TIMER_TICK   1

// 托盘
#define WM_TRAYICON     (WM_USER + 1)
#define IDM_SHOW        2001
#define IDM_STARTSTOP   2002
#define IDM_CLOSETRAY   2003
#define IDM_EXIT        2004
#define IDM_RESET       2005
#define TRAY_UID        1

// 程序图标资源（app.rc 里定义）
#ifndef IDI_APPICON
#define IDI_APPICON     101
#endif

// 供外部/自动化设置时长（lp = 秒数）
#define WM_APP_SET_SECONDS  (WM_APP + 100)
#define WM_APP_START        (WM_APP + 101)

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
enum Mode { MODE_IDLE, MODE_RUN, MODE_PAUSE, MODE_DONE };

static HINSTANCE g_hInst = nullptr;
static HWND  g_hwnd = nullptr;
static HFONT g_fontBig = nullptr;      // 大号时间
static HFONT g_fontUI = nullptr;       // 普通控件
static HFONT g_fontSmall = nullptr;    // 小字
static HBRUSH g_bgBrush = nullptr;     // 窗口背景
static int   g_dpi = 96;

// 双缓冲（复用，避免每帧重建位图导致闪烁/卡顿）
static HDC     g_memDC = nullptr;
static HBITMAP g_memBmp = nullptr;
static HGDIOBJ g_memOldBmp = nullptr;
static int     g_memW = 0, g_memH = 0;

static Mode      g_mode = MODE_IDLE;
static long long g_totalMs = 0;        // 总时长（毫秒）
static long long g_elapsedMs = 0;      // 已过（毫秒）
static Clock::time_point g_lastTick;

static wstring g_soundFile = L"alarm.wav";
static bool    g_loopSound = true;
static bool    g_popup = true;
static bool    g_flash = true;
static wstring g_popupTitle = L"倒计时结束";
static wstring g_popupMsg = L"时间到！\n\n倒计时 {time} 已经结束。";

static HWND g_edDay = nullptr, g_edHour = nullptr, g_edMin = nullptr, g_edSec = nullptr;
static HWND g_btnStart = nullptr, g_btnPause = nullptr, g_btnReset = nullptr, g_btnSound = nullptr;
static HWND g_chkPopup = nullptr, g_lblSound = nullptr;

// 托盘相关
static NOTIFYICONDATAW g_nid{};
static bool  g_trayAdded = false;
static bool  g_closeToTray = true;      // 关闭窗口时：true = 最小化到托盘，false = 直接退出
static HICON g_iconLarge = nullptr;
static HICON g_iconSmall = nullptr;
static long long g_lastTipSec = -1;     // 上次写入托盘提示的秒数
static bool  g_quitting = false;        // 真正退出标志

// 颜色
static const COLORREF C_BG      = RGB(255, 255, 255);
static const COLORREF C_TEXT    = RGB(32, 34, 40);
static const COLORREF C_MUTED   = RGB(150, 155, 165);
static const COLORREF C_ACCENT  = RGB(0, 122, 204);
static const COLORREF C_PAUSE   = RGB(230, 140, 20);
static const COLORREF C_DANGER  = RGB(214, 48, 49);
static const COLORREF C_TRACK   = RGB(236, 238, 242);
static const COLORREF C_GREEN   = RGB(38, 160, 90);

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// 前置声明
static void startCountdown();
static void togglePause();
static void resetCountdown();
static void showMainWindow();
static void invalidateDynamic(HWND hwnd);

// ---------------------------------------------------------------------------
//  声音播放（wav / mp3 / 系统别名）
// ---------------------------------------------------------------------------
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

static wstring toLowerW(wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool endsWithW(const wstring& s, const wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

class SoundPlayer {
public:
    ~SoundPlayer() { stop(); }

    void play(wstring src, bool loop) {
        stop();
        if (src.empty()) {
            PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
            m_playing = true;
            return;
        }
        if (!fileExists(src)) {
            wstring alt = exeDir() + L"\\" + src;
            if (fileExists(alt)) src = alt;
        }
        if (!fileExists(src) && src.find(L'.') == wstring::npos) {   // 系统声音别名
            PlaySoundW(src.c_str(), nullptr, SND_ALIAS | SND_ASYNC | (loop ? SND_LOOP : 0));
            m_playing = true;
            return;
        }
        if (endsWithW(toLowerW(src), L".wav")) {
            if (PlaySoundW(src.c_str(), nullptr,
                           SND_FILENAME | SND_ASYNC | SND_NODEFAULT | (loop ? SND_LOOP : 0))) {
                m_playing = true;
                return;
            }
        }
        if (mciPlay(src, loop)) { m_playing = true; return; }

        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
        m_playing = true;
    }

    void stop() {
        if (!m_playing) return;
        PlaySoundW(nullptr, nullptr, 0);
        mciSendStringW(L"close cdgui_snd", nullptr, 0, nullptr);
        m_playing = false;
    }

private:
    bool m_playing = false;

    bool mciPlay(const wstring& file, bool loop) {
        mciSendStringW(L"close cdgui_snd", nullptr, 0, nullptr);
        wstring cmd = L"open \"" + file + L"\" alias cdgui_snd";
        if (mciSendStringW(cmd.c_str(), nullptr, 0, nullptr) != 0) return false;
        if (mciSendStringW(loop ? L"play cdgui_snd repeat" : L"play cdgui_snd",
                           nullptr, 0, nullptr) != 0) {
            mciSendStringW(L"play cdgui_snd", nullptr, 0, nullptr);
        }
        return true;
    }
};

static SoundPlayer g_sound;

// ---------------------------------------------------------------------------
//  timer.ini 配置
// ---------------------------------------------------------------------------
static wstring trimW(const wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void loadConfig() {
    wstring path = exeDir() + L"\\timer.ini";
    if (!fileExists(path)) return;

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string bytes;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    fclose(f);

    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes = bytes.substr(3);

    wstring text = widen(bytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        wstring line = trimW(text.substr(pos, eol == wstring::npos ? wstring::npos : eol - pos));
        pos = (eol == wstring::npos) ? text.size() : eol + 1;
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

        if      (key == L"sound")   g_soundFile  = val;
        else if (key == L"loop")    g_loopSound  = truthy(val);
        else if (key == L"title")   g_popupTitle = val;
        else if (key == L"message") g_popupMsg   = val;
        else if (key == L"popup")   g_popup      = truthy(val);
        else if (key == L"flash")   g_flash      = truthy(val);
    }

    // message 中的 \n 转成真换行
    wstring msg;
    for (size_t i = 0; i < g_popupMsg.size(); ++i) {
        if (g_popupMsg[i] == L'\\' && i + 1 < g_popupMsg.size() && g_popupMsg[i + 1] == L'n') {
            msg += L'\n'; ++i;
        } else msg += g_popupMsg[i];
    }
    g_popupMsg = msg;
}

// ---------------------------------------------------------------------------
//  时间格式化
// ---------------------------------------------------------------------------
static wstring fmtHMS(long long sec) {
    if (sec < 0) sec = 0;
    long long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    wchar_t buf[64];
    swprintf(buf, 64, L"%02lld:%02lld:%02lld", h, m, s);
    return buf;
}

static wstring fmtHuman(long long sec) {
    long long d = sec / 86400;
    long long rest = sec % 86400;
    long long h = rest / 3600, m = (rest % 3600) / 60, s = rest % 60;
    wstring out;
    wchar_t buf[128];
    if (d > 0) {
        swprintf(buf, 128, L"%lld 天 %lld 小时 %lld 分 %lld 秒", d, h, m, s);
    } else if (h > 0) {
        swprintf(buf, 128, L"%lld 小时 %lld 分 %lld 秒", h, m, s);
    } else if (m > 0) {
        swprintf(buf, 128, L"%lld 分 %lld 秒", m, s);
    } else {
        swprintf(buf, 128, L"%lld 秒", s);
    }
    return buf;
}

// 公历日期序号（Howard Hinnant 算法），用于判断相差几天
static long long civilDayIndex(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe;
}

// 从现在起 afterSec 秒之后的目标时刻，例如 "23:55:30" / "明天 08:30:00"
static wstring fmtTargetTime(long long afterSec) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)(afterSec > 0 ? afterSec : 0) * 10000000ULL;

    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    FILETIME localFt;
    FileTimeToLocalFileTime(&ft, &localFt);
    SYSTEMTIME target;
    FileTimeToSystemTime(&localFt, &target);

    SYSTEMTIME now;
    GetLocalTime(&now);

    long long dayDiff = civilDayIndex(target.wYear, target.wMonth, target.wDay) -
                        civilDayIndex(now.wYear, now.wMonth, now.wDay);

    wchar_t buf[64];
    if (dayDiff == 0) {
        swprintf(buf, 64, L"%02d:%02d:%02d",
                 target.wHour, target.wMinute, target.wSecond);
    } else if (dayDiff == 1) {
        swprintf(buf, 64, L"明天 %02d:%02d:%02d",
                 target.wHour, target.wMinute, target.wSecond);
    } else if (dayDiff == 2) {
        swprintf(buf, 64, L"后天 %02d:%02d:%02d",
                 target.wHour, target.wMinute, target.wSecond);
    } else {
        swprintf(buf, 64, L"%d-%02d-%02d %02d:%02d:%02d",
                 target.wYear, target.wMonth, target.wDay,
                 target.wHour, target.wMinute, target.wSecond);
    }
    return buf;
}

// ---------------------------------------------------------------------------
//  读取输入框里的时长
// ---------------------------------------------------------------------------
static int getEditInt(HWND hEdit, int defVal) {
    wchar_t buf[32] = { 0 };
    GetWindowTextW(hEdit, buf, 32);
    if (!buf[0]) return defVal;
    int v = _wtoi(buf);
    if (v < 0) v = 0;
    return v;
}

static long long readDurationMs() {
    long long d = getEditInt(g_edDay, 0);
    long long h = getEditInt(g_edHour, 0);
    long long m = getEditInt(g_edMin, 0);
    long long s = getEditInt(g_edSec, 0);
    return ((d * 86400 + h * 3600 + m * 60 + s) * 1000LL);
}

// ---------------------------------------------------------------------------
//  托盘图标与菜单
// ---------------------------------------------------------------------------
// 用 GDI 画一个"时钟"图标，避免依赖外部 .ico 资源文件
static HICON createAppIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(hdc); return nullptr; }

    HGDIOBJ oldBmp = SelectObject(hdc, dib);
    memset(bits, 0, (size_t)size * (size_t)size * 4);

    // 圆角方形底 + 表盘
    HBRUSH br = CreateSolidBrush(RGB(0, 122, 204));
    HPEN   np = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, np);
    Ellipse(hdc, 1, 1, size - 1, size - 1);

    // 指针
    int thick = (size >= 32) ? 3 : 2;
    HPEN wp = CreatePen(PS_SOLID, thick, RGB(255, 255, 255));
    HGDIOBJ ow = SelectObject(hdc, wp);
    int cx = size / 2, cy = size / 2;
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, cx, size / 4);
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, size * 3 / 4, cy + size / 8);

    SelectObject(hdc, ow);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(br);
    DeleteObject(np);
    DeleteObject(wp);
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);

    // GDI 绘制不写 alpha 通道，这里手动补：非纯黑像素视为不透明
    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < size * size; ++i) {
        if ((px[i] & 0x00FFFFFF) != 0) px[i] |= 0xFF000000;
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = dib;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(dib);
    return icon;
}

// 记住"关闭时最小化到托盘"的偏好
static void loadTraySetting() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\CppCountdown", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"CloseToTray", nullptr, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD)
            g_closeToTray = (v != 0);
        RegCloseKey(k);
    }
}

static void saveTraySetting() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CppCountdown", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = g_closeToTray ? 1 : 0;
        RegSetValueExW(k, L"CloseToTray", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void addTrayIcon() {
    if (g_trayAdded) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_iconSmall ? g_iconSmall : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"C++ 倒计时器");
    g_trayAdded = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
}

static void removeTrayIcon() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

// 更新托盘提示文字（只在秒数变化时刷新）
static void updateTrayTip(long long remainSec) {
    if (!g_trayAdded) return;
    if (remainSec == g_lastTipSec) return;
    g_lastTipSec = remainSec;

    wstring tip;
    if (g_mode == MODE_RUN || g_mode == MODE_PAUSE) {
        tip = L"倒计时 " + fmtHMS(remainSec);
        if (g_mode == MODE_PAUSE) tip += L"（已暂停）";
    } else {
        tip = L"C++ 倒计时器";
    }
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void hideToTray() {
    ShowWindow(g_hwnd, SW_HIDE);

    // 第一次收进托盘时给个气泡提示，避免用户以为程序退出了
    static bool showedHint = false;
    if (!showedHint && g_trayAdded) {
        showedHint = true;
        NOTIFYICONDATAW nid = g_nid;
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"C++ 倒计时器");
        wcscpy_s(nid.szInfo, L"程序已最小化到托盘。双击托盘图标可重新打开，右键可退出。");
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static void showMainWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void showTrayMenu() {
    HMENU menu = CreatePopupMenu();

    wstring startText;
    if (g_mode == MODE_RUN)       startText = L"暂停倒计时";
    else if (g_mode == MODE_PAUSE) startText = L"继续倒计时";
    else                          startText = L"开始倒计时";

    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"显示主窗口");
    AppendMenuW(menu, MF_STRING, IDM_STARTSTOP, startText.c_str());
    AppendMenuW(menu, MF_STRING | (g_mode == MODE_IDLE ? MF_GRAYED : 0), IDM_RESET, L"重置倒计时");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_closeToTray ? MF_CHECKED : MF_UNCHECKED),
                IDM_CLOSETRAY, L"关闭窗口时最小化到托盘");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出程序");

    // 必须先让窗口成为前台，否则菜单点外面不会消失
    SetForegroundWindow(g_hwnd);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}


static void setEditEnabled(bool on) {
    EnableWindow(g_edDay, on);
    EnableWindow(g_edHour, on);
    EnableWindow(g_edMin, on);
    EnableWindow(g_edSec, on);
    EnableWindow(g_btnSound, on);
}

static void updateUI() {
    switch (g_mode) {
        case MODE_IDLE:
            setEditEnabled(true);
            EnableWindow(g_btnStart, TRUE);
            EnableWindow(g_btnPause, FALSE);
            EnableWindow(g_btnReset, FALSE);
            SetWindowTextW(g_btnPause, L"暂停");
            break;
        case MODE_RUN:
            setEditEnabled(false);
            EnableWindow(g_btnStart, FALSE);
            EnableWindow(g_btnPause, TRUE);
            EnableWindow(g_btnReset, TRUE);
            SetWindowTextW(g_btnPause, L"暂停");
            break;
        case MODE_PAUSE:
            setEditEnabled(false);
            EnableWindow(g_btnStart, FALSE);
            EnableWindow(g_btnPause, TRUE);
            EnableWindow(g_btnReset, TRUE);
            SetWindowTextW(g_btnPause, L"继续");
            break;
        case MODE_DONE:
            setEditEnabled(true);
            EnableWindow(g_btnStart, TRUE);
            EnableWindow(g_btnPause, FALSE);
            EnableWindow(g_btnReset, TRUE);
            SetWindowTextW(g_btnPause, L"暂停");
            break;
    }
    invalidateDynamic(g_hwnd);
}

static void updateSoundLabel() {
    wstring name = g_soundFile.empty() ? L"(系统提示音)" : g_soundFile;
    // 只显示文件名
    size_t pos = name.find_last_of(L"\\/");
    if (pos != wstring::npos && pos + 1 < name.size()) name = name.substr(pos + 1);
    wstring text = L"提示音：" + name;
    if (text.size() > 34) text = text.substr(0, 33) + L"...";
    SetWindowTextW(g_lblSound, text.c_str());
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------
static void fillRoundRect(HDC hdc, RECT rc, int radius, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// 释放双缓冲资源
static void releaseBackBuffer() {
    if (g_memDC) {
        if (g_memOldBmp) SelectObject(g_memDC, g_memOldBmp);
        if (g_memBmp) DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    g_memDC = nullptr;
    g_memBmp = nullptr;
    g_memOldBmp = nullptr;
    g_memW = g_memH = 0;
}

// 按当前客户区尺寸准备双缓冲（尺寸没变就直接复用）
static bool ensureBackBuffer(HDC hdcRef, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_memDC && g_memW == w && g_memH == h) return true;

    releaseBackBuffer();
    g_memDC = CreateCompatibleDC(hdcRef);
    if (!g_memDC) return false;
    g_memBmp = CreateCompatibleBitmap(hdcRef, w, h);
    if (!g_memBmp) {
        DeleteDC(g_memDC);
        g_memDC = nullptr;
        return false;
    }
    g_memOldBmp = SelectObject(g_memDC, g_memBmp);
    g_memW = w;
    g_memH = h;
    return true;
}

// 只需要重绘上半部分的动态内容（大数字 / 副标题 / 进度条 / 分隔线），
// 下半部分是子控件，不参与父窗口重绘，减少绘制面积
static void invalidateDynamic(HWND hwnd) {
    RECT rc{ 0, 0, S(520), S(202) };
    InvalidateRect(hwnd, &rc, FALSE);
}

static void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdcReal = BeginPaint(hwnd, &ps);

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int W = rcClient.right, H = rcClient.bottom;

    // 双缓冲：先在内存位图上画完，再一次性贴到屏幕
    if (!ensureBackBuffer(hdcReal, W, H)) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC hdc = g_memDC;

    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(hdc, &rcClient, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    // ---------- 计算要显示的内容 ----------
    long long remainMs = g_totalMs - g_elapsedMs;
    if (remainMs < 0) remainMs = 0;
    long long remainSec = (remainMs + 999) / 1000;

    wstring bigText;
    COLORREF bigColor;
    wstring subText;

    if (g_mode == MODE_IDLE) {
        long long inputMs = readDurationMs();
        bigText = fmtHMS((inputMs + 999) / 1000);
        bigColor = (inputMs > 0) ? C_TEXT : C_MUTED;
        if (inputMs > 0) {
            subText = L"将倒计时 " + fmtHuman(inputMs / 1000) +
                      L"　·　预计 " + fmtTargetTime(inputMs / 1000) + L" 响铃";
        } else {
            subText = L"请设置倒计时时长";
        }
    } else if (g_mode == MODE_DONE) {
        bigText = L"00:00:00";
        bigColor = C_DANGER;
        subText = L"时间到！";
    } else {
        bigText = fmtHMS(remainSec);
        bigColor = (g_mode == MODE_PAUSE) ? C_PAUSE : C_ACCENT;
        wstring target = fmtTargetTime(remainSec);
        if (g_mode == MODE_PAUSE) {
            subText = L"已暂停　·　预计 " + target + L" 响铃";
        } else {
            subText = L"剩余 " + fmtHuman(remainSec) + L"　·　预计 " + target + L" 响铃";
        }
    }

    // ---------- 大号时间 ----------
    SelectObject(hdc, g_fontBig);
    SetTextColor(hdc, bigColor);
    RECT rcBig{ 0, S(28), W, S(28) + S(96) };
    DrawTextW(hdc, bigText.c_str(), -1, &rcBig, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---------- 副标题 ----------
    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, g_mode == MODE_DONE ? C_DANGER : C_MUTED);
    RECT rcSub{ 0, S(122), W, S(122) + S(24) };
    DrawTextW(hdc, subText.c_str(), -1, &rcSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---------- 进度条 ----------
    int barL = S(48), barR = W - S(48);
    int barT = S(158), barB = barT + S(14);
    RECT rcTrack{ barL, barT, barR, barB };
    fillRoundRect(hdc, rcTrack, S(14), C_TRACK);

    double ratio = 0.0;
    if (g_mode == MODE_DONE) ratio = 1.0;
    else if (g_totalMs > 0 && g_mode != MODE_IDLE)
        ratio = (double)g_elapsedMs / (double)g_totalMs;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    if (ratio > 0.001) {
        int fillR = barL + (int)std::lround((barR - barL) * ratio);
        if (fillR - barL < S(14)) fillR = barL + S(14);
        RECT rcFill{ barL, barT, fillR, barB };
        COLORREF fc = (g_mode == MODE_DONE) ? C_DANGER
                    : (g_mode == MODE_PAUSE) ? C_PAUSE : C_ACCENT;
        fillRoundRect(hdc, rcFill, S(14), fc);
    }

    // ---------- 百分比 ----------
    if (g_mode != MODE_IDLE) {
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", (int)std::lround(ratio * 100));
        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, C_MUTED);
        RECT rcPct{ barL, barB + S(4), barR, barB + S(24) };
        DrawTextW(hdc, pct, -1, &rcPct, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }

    // ---------- 分隔线 ----------
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(238, 240, 244));
        HGDIOBJ op = SelectObject(hdc, pen);
        MoveToEx(hdc, S(32), S(196), nullptr);
        LineTo(hdc, W - S(32), S(196));
        SelectObject(hdc, op);
        DeleteObject(pen);
    }

    BitBlt(hdcReal, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
//  开始 / 暂停 / 重置 / 结束
// ---------------------------------------------------------------------------
static void startCountdown() {
    long long ms = readDurationMs();
    if (ms <= 0) {
        MessageBoxW(g_hwnd, L"请先设置一个大于 0 的倒计时时长。", L"提示",
                    MB_OK | MB_ICONWARNING);
        SetFocus(g_edSec);
        return;
    }
    g_totalMs = ms;
    g_elapsedMs = 0;
    g_mode = MODE_RUN;
    g_lastTick = Clock::now();
    SetTimer(g_hwnd, ID_TIMER_TICK, 50, nullptr);
    updateUI();
}

static void togglePause() {
    if (g_mode == MODE_RUN) {
        g_mode = MODE_PAUSE;
        KillTimer(g_hwnd, ID_TIMER_TICK);
    } else if (g_mode == MODE_PAUSE) {
        g_mode = MODE_RUN;
        g_lastTick = Clock::now();
        SetTimer(g_hwnd, ID_TIMER_TICK, 50, nullptr);
    }
    updateUI();
}

static void resetCountdown() {
    KillTimer(g_hwnd, ID_TIMER_TICK);
    g_sound.stop();
    g_mode = MODE_IDLE;
    g_totalMs = 0;
    g_elapsedMs = 0;
    updateUI();
}

static void finishCountdown() {
    KillTimer(g_hwnd, ID_TIMER_TICK);
    g_mode = MODE_DONE;
    g_elapsedMs = g_totalMs;
    updateUI();
    updateTrayTip(0);

    // 如果窗口被收在托盘里，先弹出来提醒
    if (!IsWindowVisible(g_hwnd)) showMainWindow();

    // 任务栏闪烁
    if (g_flash) {
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = g_hwnd;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fi.uCount = 0;
        FlashWindowEx(&fi);
    }

    // 声音
    g_sound.play(g_soundFile, g_loopSound);

    // 弹窗
    if (g_popup) {
        wstring msg = g_popupMsg;
        wstring ph = L"{time}";
        for (size_t p = msg.find(ph); p != wstring::npos; p = msg.find(ph, p))
            msg.replace(p, ph.size(), fmtHuman(g_totalMs / 1000));

        MessageBoxW(g_hwnd, msg.c_str(), g_popupTitle.c_str(),
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }

    g_sound.stop();

    if (g_flash) {
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = g_hwnd;
        fi.dwFlags = FLASHW_STOP;
        FlashWindowEx(&fi);
    }

    // 结束后回到可编辑状态，方便再来一次
    g_mode = MODE_IDLE;
    g_totalMs = 0;
    g_elapsedMs = 0;
    updateUI();
    SetForegroundWindow(g_hwnd);
}

// ---------------------------------------------------------------------------
//  创建控件
// ---------------------------------------------------------------------------
static HWND makeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                      HFONT font, int align) {
    HWND ctrl = CreateWindowExW(0, L"STATIC", text,
                                WS_CHILD | WS_VISIBLE | align,
                                S(x), S(y), S(w), S(h),
                                parent, nullptr, g_hInst, nullptr);
    SendMessageW(ctrl, WM_SETFONT, (WPARAM)font, TRUE);
    return ctrl;
}

static HWND makeEdit(HWND parent, int id, int x, int y, int w, int h) {
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                             WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
                             S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(e, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
    return e;
}

static HWND makeButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND b = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
    return b;
}

static void createControls(HWND hwnd) {
    // 输入框组：4 个框 + 3 个冒号
    const int boxW = 74, boxH = 34, gap = 26;
    int totalW = 4 * boxW + 3 * gap;
    int startX = (520 - totalW) / 2;
    int boxY = 232;

    const wchar_t* names[4] = { L"天", L"小时", L"分钟", L"秒" };
    HWND* edits[4] = { &g_edDay, &g_edHour, &g_edMin, &g_edSec };
    int ids[4] = { ID_EDIT_DAY, ID_EDIT_HOUR, ID_EDIT_MIN, ID_EDIT_SEC };

    for (int i = 0; i < 4; ++i) {
        int x = startX + i * (boxW + gap);
        makeLabel(hwnd, names[i], x, boxY - 24, boxW, 20, g_fontSmall, SS_CENTER);
        *edits[i] = makeEdit(hwnd, ids[i], x, boxY, boxW, boxH);
        if (i < 3) {
            makeLabel(hwnd, L":", x + boxW, boxY + 4, gap, 24, g_fontUI, SS_CENTER);
        }
    }

    // 按钮行
    int btnY = 292, btnH = 36, btnW = 112, btnGap = 18;
    int btnTotal = 3 * btnW + 2 * btnGap;
    int btnX = (520 - btnTotal) / 2;

    g_btnStart = makeButton(hwnd, ID_BTN_START, L"开 始", btnX, btnY, btnW, btnH);
    g_btnPause = makeButton(hwnd, ID_BTN_PAUSE, L"暂 停", btnX + btnW + btnGap, btnY, btnW, btnH);
    g_btnReset = makeButton(hwnd, ID_BTN_RESET, L"重 置", btnX + 2 * (btnW + btnGap), btnY, btnW, btnH);

    // 声音行
    g_lblSound = makeLabel(hwnd, L"提示音：", 44, 352, 320, 22, g_fontSmall, SS_LEFT);
    g_btnSound = makeButton(hwnd, ID_BTN_SOUND, L"浏览...", 380, 348, 96, 28);

    // 弹窗复选框
    g_chkPopup = CreateWindowExW(0, L"BUTTON", L"结束时弹出提示窗口",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 S(44), S(386), S(240), S(24),
                                 hwnd, (HMENU)(INT_PTR)ID_CHK_POPUP, g_hInst, nullptr);
    SendMessageW(g_chkPopup, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    SendMessageW(g_chkPopup, BM_SETCHECK, g_popup ? BST_CHECKED : BST_UNCHECKED, 0);

    updateSoundLabel();
    updateUI();
}

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        createControls(hwnd);
        return 0;

    case WM_PAINT:
        paintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;      // 背景在 WM_PAINT 里统一绘制

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_bgBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB(250, 251, 253));
        SetTextColor(hdc, C_TEXT);
        static HBRUSH editBrush = CreateSolidBrush(RGB(250, 251, 253));
        return (LRESULT)editBrush;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_TICK && g_mode == MODE_RUN) {
            auto now = Clock::now();
            long long dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastTick).count();
            g_lastTick = now;
            if (dt < 0) dt = 0;
            if (dt > 1000) dt = 1000;      // 防止休眠唤醒后跳变
            g_elapsedMs += dt;

            if (g_elapsedMs >= g_totalMs) {
                finishCountdown();
            } else {
                long long remain = (g_totalMs - g_elapsedMs + 999) / 1000;
                updateTrayTip(remain);
                invalidateDynamic(hwnd);
            }
        }
        return 0;

    case WM_SYSCOMMAND:
        // 最小化按钮 -> 收进托盘（托盘不可用时按普通最小化处理）
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            if (g_trayAdded) {
                hideToTray();
                return 0;
            }
            break;
        }
        break;

    case WM_TRAYICON: {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            showMainWindow();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            showTrayMenu();
        }
        return 0;
    }

    // 外部设置时长（秒），便于自动化或二次开发
    case WM_APP_SET_SECONDS: {
        long long sec = (long long)lp;
        if (sec < 0) sec = 0;
        wchar_t buf[32];
        swprintf(buf, 32, L"%lld", sec / 86400);          SetWindowTextW(g_edDay, buf);
        swprintf(buf, 32, L"%lld", (sec % 86400) / 3600); SetWindowTextW(g_edHour, buf);
        swprintf(buf, 32, L"%lld", (sec % 3600) / 60);    SetWindowTextW(g_edMin, buf);
        swprintf(buf, 32, L"%lld", sec % 60);             SetWindowTextW(g_edSec, buf);
        invalidateDynamic(hwnd);
        return 0;
    }

    case WM_APP_START:
        if (g_mode == MODE_IDLE) startCountdown();
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        // ---- 托盘菜单命令 ----
        if (id == IDM_SHOW) {
            showMainWindow();
            return 0;
        }
        if (id == IDM_STARTSTOP) {
            if (g_mode == MODE_IDLE) startCountdown();
            else togglePause();
            return 0;
        }
        if (id == IDM_RESET) {
            resetCountdown();
            return 0;
        }
        if (id == IDM_CLOSETRAY) {
            g_closeToTray = !g_closeToTray;
            saveTraySetting();
            return 0;
        }
        if (id == IDM_EXIT) {
            g_quitting = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }

        // ---- 窗口控件 ----
        if (id == ID_BTN_START && code == BN_CLICKED) {
            if (g_mode == MODE_IDLE) startCountdown();
        } else if (id == ID_BTN_PAUSE && code == BN_CLICKED) {
            togglePause();
        } else if (id == ID_BTN_RESET && code == BN_CLICKED) {
            resetCountdown();
        } else if (id == ID_BTN_SOUND && code == BN_CLICKED) {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"音频文件 (*.wav;*.mp3)\0*.wav;*.mp3\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"选择提示音";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_soundFile = file;
                updateSoundLabel();
            }
        } else if (id == ID_CHK_POPUP && code == BN_CLICKED) {
            g_popup = (SendMessageW(g_chkPopup, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if ((id == ID_EDIT_DAY || id == ID_EDIT_HOUR ||
                    id == ID_EDIT_MIN || id == ID_EDIT_SEC) && code == EN_CHANGE) {
            if (g_mode == MODE_IDLE) invalidateDynamic(hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        // 默认收进托盘；托盘不可用或在托盘菜单里取消勾选后则直接退出
        if (g_closeToTray && !g_quitting && g_trayAdded) {
            hideToTray();
            return 0;
        }
        g_sound.stop();
        removeTrayIcon();
        DestroyWindow(hwnd);
        return 0;

    case WM_SIZE:
        // 尺寸变化时丢弃旧的双缓冲，下次绘制时按新尺寸重建
        releaseBackBuffer();
        return 0;

    case WM_DESTROY:
        removeTrayIcon();
        releaseBackBuffer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (g_dpi <= 0) g_dpi = 96;

    loadConfig();
    loadTraySetting();

    // 图标：优先用编译进 exe 资源的 app.ico，加载失败时退回 GDI 现画
    g_iconLarge = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                    LR_DEFAULTCOLOR);
    if (!g_iconLarge) g_iconLarge = createAppIcon(GetSystemMetrics(SM_CXICON));

    g_iconSmall = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                    LR_DEFAULTCOLOR);
    if (!g_iconSmall) g_iconSmall = createAppIcon(GetSystemMetrics(SM_CXSMICON));

    // 字体
    g_fontBig = CreateFontW(-S(76), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontUI = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_bgBrush = CreateSolidBrush(C_BG);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"CountdownGuiClass";
    wc.hIcon = g_iconLarge ? g_iconLarge : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = g_iconSmall ? g_iconSmall : LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    // 窗口尺寸（含边框）
    int w = S(520), h = S(444);
    RECT rc{ 0, 0, w, h };
    // WS_CLIPCHILDREN：父窗口绘制时跳过子控件区域，避免控件区域被反复覆盖而闪烁
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    g_hwnd = CreateWindowExW(0, L"CountdownGuiClass", L"C++ 倒计时器",
                             style,
                             x, y, ww, wh,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    addTrayIcon();
    SetFocus(g_edSec);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // 空格 = 开始/暂停（焦点在编辑框时除外）
        if (msg.message == WM_KEYDOWN) {
            HWND focus = GetFocus();
            wchar_t cls[32] = { 0 };
            if (focus) GetClassNameW(focus, cls, 32);
            bool inEdit = (wcscmp(cls, L"Edit") == 0);

            if (msg.wParam == VK_ESCAPE) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                continue;
            }
            if (msg.wParam == VK_SPACE && !inEdit) {
                if (g_mode == MODE_IDLE) startCountdown();
                else if (g_mode == MODE_RUN || g_mode == MODE_PAUSE) togglePause();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_sound.stop();
    removeTrayIcon();
    if (g_iconLarge) DestroyIcon(g_iconLarge);
    if (g_iconSmall) DestroyIcon(g_iconSmall);
    if (g_fontBig) DeleteObject(g_fontBig);
    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    return 0;
}
