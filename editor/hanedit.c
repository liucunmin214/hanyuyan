/*
 * 汉语言编辑器 hanedit —— 纯 Win32 桌面编辑器，无第三方依赖
 *
 * 功能：
 *   - 新建/打开/保存/另存为 .汉 源文件（UTF-8 编码）
 *   - 语法高亮：中文关键字、类型名、字符串、数字、注释（# 与嵌套块注释）
 *   - F5 一键编译运行：调用同目录 han.exe，输出捕获到下方输出面板
 *   - 编译错误按「文件:行:列」解析，输出面板双击错误行跳转到源码行
 *   - F6 仅语法检查；F1 打开帮助教程；支持命令行参数与文件拖放打开
 *
 * 构建：gcc -std=c11 -O2 -Wall -Wextra -mwindows editor/hanedit.c -o build/hanedit.exe
 */

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------------- 控件与菜单 ID ---------------- */
#define ID_EDIT      101
#define ID_OUT       102
#define ID_STATUS    103

#define IDM_NEW      1001
#define IDM_OPEN     1002
#define IDM_SAVE     1003
#define IDM_SAVEAS   1004
#define IDM_EXIT     1005
#define IDM_UNDO     1101
#define IDM_REDO     1102
#define IDM_CUT      1103
#define IDM_COPY     1104
#define IDM_PASTE    1105
#define IDM_SELALL   1106
#define IDM_RUN      1201
#define IDM_CHECK    1202
#define IDM_HELP     1301
#define IDM_ABOUT    1302
#define IDM_LOCATE   1303
#define IDM_MANUAL   1304

#define IDT_HIGHLIGHT 1
#define SPLIT_H       4
#define OUT_MIN       60
#define RUN_TIMEOUT_MS 30000

/* ---------------- 高亮配色 ---------------- */
#define CLR_TEXT    RGB(0x20, 0x20, 0x20)
#define CLR_KW      RGB(0x00, 0x33, 0xCC)   /* 关键字：蓝 */
#define CLR_TYPE    RGB(0x6F, 0x00, 0xD2)   /* 类型名：紫 */
#define CLR_STRING  RGB(0xC0, 0x50, 0x00)   /* 字符串：橙 */
#define CLR_NUMBER  RGB(0x00, 0x80, 0x80)   /* 数字：青 */
#define CLR_COMMENT RGB(0x00, 0x80, 0x00)   /* 注释：绿 */
#define CLR_ERROR   RGB(0xCC, 0x00, 0x00)   /* 输出面板错误行：红 */
#define CLR_OK      RGB(0x00, 0x80, 0x00)   /* 输出面板成功行：绿 */

/* 关键字表（与 src/lexer/keywords.c 同义词表保持一致，另含上下文词「到」） */
static const WCHAR *KW_TABLE[] = {
    L"函数", L"定义", L"返回", L"变量", L"令", L"常量",
    L"如果", L"若", L"否则", L"不然",
    L"当", L"循环", L"只要", L"对于", L"遍历", L"到",
    L"结束", L"完了", L"打印", L"输出", L"显示",
    L"真", L"是", L"假", L"否", L"空", L"无",
    L"与", L"且", L"或", L"非", L"跳出", L"停止", L"继续", L"跳过",
    L"导入", L"引用", L"从",
};

/* 内置类型名 */
static const WCHAR *TYPE_TABLE[] = {
    L"整数", L"小数", L"文字", L"布尔", L"列表", L"字典",
};

/* ---------------- 全局状态 ---------------- */
static HWND  g_hwnd, g_edit, g_out, g_status;
static WCHAR g_filePath[MAX_PATH] = L"";
static BOOL  g_modified = FALSE;
static BOOL  g_loading  = FALSE;   /* 装载文本期间屏蔽修改标记 */
static int   g_outHeight = 170;
static BOOL  g_dragging = FALSE;
static WCHAR g_hanPath[MAX_PATH] = L"";   /* 已定位到的 han.exe 全路径 */

/* ---------------- 小工具 ---------------- */

static BOOL isIdentStart(WCHAR c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
           c == 0x3007 ||
           (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}

static BOOL isIdentChar(WCHAR c) {
    return isIdentStart(c) || (c >= L'0' && c <= L'9');
}

static BOOL inTable(const WCHAR *word, DWORD len, const WCHAR **table, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const WCHAR *kw = table[i];
        if (wcslen(kw) == len && wmemcmp(kw, word, len) == 0) return TRUE;
    }
    return FALSE;
}

/* 给编辑区 [a,b) 区间设置文字颜色 */
static void setColor(DWORD a, DWORD b, COLORREF color) {
    CHARRANGE cr;
    CHARFORMAT2W cf;
    cr.cpMin = (LONG)a;
    cr.cpMax = (LONG)b;
    SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.dwEffects = 0;
    cf.crTextColor = color;
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

/* 全量语法高亮（小文件足够快；输入时经 250ms 防抖触发） */
static void highlight(void) {
    GETTEXTLENGTHEX gtl;
    GETTEXTEX gt;
    DWORD len, i;
    WCHAR *t;
    DWORD mask;
    CHARRANGE old;

    memset(&gtl, 0, sizeof(gtl));
    gtl.flags = GTL_DEFAULT;
    gtl.codepage = 1200;
    len = (DWORD)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len == 0 || len > (1u << 20)) return;

    t = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 2) * sizeof(WCHAR));
    if (!t) return;
    memset(&gt, 0, sizeof(gt));
    gt.cb = (len + 1) * sizeof(WCHAR);
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    SendMessageW(g_edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)t);

    SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
    mask = (DWORD)SendMessageW(g_edit, EM_SETEVENTMASK, 0, 0);
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&old);

    setColor(0, len, CLR_TEXT);

    i = 0;
    while (i < len) {
        WCHAR c = t[i];
        DWORD j;
        if (c == L'#') {                                  /* 行注释 */
            j = i;
            while (j < len && t[j] != L'\n' && t[j] != L'\r') j++;
            setColor(i, j, CLR_COMMENT);
            i = j;
        } else if (c == L'/' && i + 1 < len && t[i + 1] == L'*') { /* 嵌套块注释 */
            int depth = 1;
            j = i + 2;
            while (j + 1 < len && depth > 0) {
                if (t[j] == L'/' && t[j + 1] == L'*') { depth++; j += 2; }
                else if (t[j] == L'*' && t[j + 1] == L'/') { depth--; j += 2; }
                else j++;
            }
            if (depth > 0) j = len;
            setColor(i, j, CLR_COMMENT);
            i = j;
        } else if (c == L'"' || c == 0x201C || c == 0x300C || c == 0x300E) { /* 字符串 */
            WCHAR close = (c == L'"') ? L'"' : (c == 0x201C) ? 0x201D :
                          (c == 0x300C) ? 0x300D : 0x300F;
            j = i + 1;
            while (j < len) {
                if (t[j] == L'\\') { j += 2; continue; }
                if (t[j] == close) { j++; break; }
                if (t[j] == L'\n') break;
                j++;
            }
            setColor(i, j, CLR_STRING);
            i = j;
        } else if (c >= L'0' && c <= L'9') {               /* 数字 */
            j = i + 1;
            while (j < len && ((t[j] >= L'0' && t[j] <= L'9') ||
                               (t[j] >= L'a' && t[j] <= L'z') ||
                               (t[j] >= L'A' && t[j] <= L'Z') ||
                               t[j] == L'.' || t[j] == L'_')) j++;
            setColor(i, j, CLR_NUMBER);
            i = j;
        } else if (isIdentStart(c)) {                      /* 标识符 / 关键字 */
            j = i + 1;
            while (j < len && isIdentChar(t[j])) j++;
            if (inTable(t + i, j - i, KW_TABLE, sizeof(KW_TABLE) / sizeof(KW_TABLE[0])))
                setColor(i, j, CLR_KW);
            else if (inTable(t + i, j - i, TYPE_TABLE, sizeof(TYPE_TABLE) / sizeof(TYPE_TABLE[0])))
                setColor(i, j, CLR_TYPE);
            i = j;
        } else {
            i++;
        }
    }

    SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&old);
    SendMessageW(g_edit, EM_SETEVENTMASK, 0, mask);
    SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_edit, NULL, FALSE);
    HeapFree(GetProcessHeap(), 0, t);
}

/* ---------------- 标题与状态栏 ---------------- */

static void updateTitle(void) {
    WCHAR buf[MAX_PATH + 64];
    const WCHAR *name = g_filePath[0] ? g_filePath : L"未命名";
    _snwprintf(buf, MAX_PATH + 64, L"%s%s - 汉语言编辑器", name, g_modified ? L" *" : L"");
    SetWindowTextW(g_hwnd, buf);
}

static void updateStatus(void) {
    CHARRANGE cr;
    DWORD line, col, lineStart;
    WCHAR buf[96], han[96];
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    line = (DWORD)SendMessageW(g_edit, EM_EXLINEFROMCHAR, (WPARAM)-1, (LPARAM)cr.cpMin);
    lineStart = (DWORD)SendMessageW(g_edit, EM_LINEINDEX, (WPARAM)line, 0);
    col = (DWORD)cr.cpMin - lineStart;
    _snwprintf(buf, 96, L" 行列：%u , %u   ", line + 1, col + 1);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)L" 编码：UTF-8   ");
    if (g_hanPath[0])
        SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)L" 编译器：已就绪   ");
    else {
        _snwprintf(han, 96, L" 编译器：未定位（F5 时可手动指定）   ");
        SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)han);
    }
}

/* ---------------- 输出面板 ---------------- */

static void outSetText(const WCHAR *text) {
    SETTEXTEX st;
    memset(&st, 0, sizeof(st));
    st.flags = ST_UNICODE;
    st.codepage = 1200;
    SendMessageW(g_out, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)text);
}

static void outAppend(const WCHAR *text) {
    CHARRANGE cr;
    SETTEXTEX st;
    DWORD len;
    memset(&cr, 0, sizeof(cr));
    len = (DWORD)GetWindowTextLengthW(g_out);
    cr.cpMin = cr.cpMax = (LONG)len;
    SendMessageW(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);
    memset(&st, 0, sizeof(st));
    st.flags = ST_UNICODE;
    st.codepage = 1200;
    SendMessageW(g_out, EM_REPLACESEL, 0, (LPARAM)text);
    (void)st;
    /* 滚动到底部 */
    SendMessageW(g_out, WM_VSCROLL, SB_BOTTOM, 0);
}

/* 输出面板着色：含「错误」的行标红，含「✓」的行标绿 */
static void outColorize(void) {
    DWORD lines = (DWORD)SendMessageW(g_out, EM_GETLINECOUNT, 0, 0);
    DWORD mask = (DWORD)SendMessageW(g_out, EM_SETEVENTMASK, 0, 0);
    CHARRANGE old;
    SendMessageW(g_out, EM_EXGETSEL, 0, (LPARAM)&old);
    SendMessageW(g_out, WM_SETREDRAW, FALSE, 0);
    for (DWORD ln = 0; ln < lines; ln++) {
        DWORD start = (DWORD)SendMessageW(g_out, EM_LINEINDEX, (WPARAM)ln, 0);
        DWORD llen = (DWORD)SendMessageW(g_out, EM_LINELENGTH, (WPARAM)start, 0);
        WCHAR buf[512];
        CHARRANGE cr;
        CHARFORMAT2W cf;
        DWORD n = llen < 510 ? llen : 510;
        WORD wn = (WORD)n;
        memcpy(buf, &wn, sizeof(WORD));           /* EM_GETLINE 首字为缓冲大小 */
        SendMessageW(g_out, EM_GETLINE, (WPARAM)ln, (LPARAM)buf);
        buf[n] = 0;
        cr.cpMin = (LONG)start;
        cr.cpMax = (LONG)(start + llen);
        SendMessageW(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        if (wcsstr(buf, L"错误") || wcsstr(buf, L"✗"))
            cf.crTextColor = CLR_ERROR;
        else if (wcsstr(buf, L"✓"))
            cf.crTextColor = CLR_OK;
        else
            cf.crTextColor = CLR_TEXT;
        SendMessageW(g_out, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    SendMessageW(g_out, EM_EXSETSEL, 0, (LPARAM)&old);
    SendMessageW(g_out, EM_SETEVENTMASK, 0, mask);
    SendMessageW(g_out, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_out, NULL, FALSE);
}

/* ---------------- 子进程执行与输出捕获 ---------------- */

/*
 * 以隐藏窗口执行 cmdline（可写缓冲），捕获 stdout+stderr（UTF-8 字节流），
 * 转为 UTF-16 经 outUtf16 返回（调用方负责 HeapFree）；返回进程退出码。
 * 超过 RUN_TIMEOUT_MS 未退出则强制终止并返回 0xFFFF。
 */
static DWORD run_capture(const WCHAR *cmdline, WCHAR **outUtf16) {
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *cmd;
    char *buf = NULL;
    size_t cap = 0, used = 0;
    DWORD code = 0xFFFF;
    DWORD waited = 0;

    *outUtf16 = NULL;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0xFFFF;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    cmd = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (wcslen(cmdline) + 1) * sizeof(WCHAR));
    if (!cmd) { CloseHandle(rd); CloseHandle(wr); return 0xFFFF; }
    wcscpy(cmd, cmdline);

    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        HeapFree(GetProcessHeap(), 0, cmd);
        CloseHandle(rd);
        CloseHandle(wr);
        return 0xFFFF;
    }
    HeapFree(GetProcessHeap(), 0, cmd);
    CloseHandle(wr);   /* 关闭写端，读端才能在进程结束后看到 EOF */

    for (;;) {
        DWORD avail = 0;
        PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL);
        if (avail > 0) {
            char tmp[4096];
            DWORD n = 0, want = avail > sizeof(tmp) ? sizeof(tmp) : avail;
            if (ReadFile(rd, tmp, want, &n, NULL) && n > 0) {
                if (used + n + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 8192;
                    char *nb;
                    while (ncap < used + n + 1) ncap *= 2;
                    nb = (char *)HeapReAlloc(GetProcessHeap(), 0, buf, ncap);
                    if (!nb) break;
                    buf = nb;
                    cap = ncap;
                }
                memcpy(buf + used, tmp, n);
                used += n;
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        waited += 100;
        if (waited > RUN_TIMEOUT_MS) {
            TerminateProcess(pi.hProcess, 1);
            code = 0xFFFE;
            break;
        }
    }
    /* 读剩余输出 */
    for (;;) {
        char tmp[4096];
        DWORD n = 0;
        if (!ReadFile(rd, tmp, sizeof(tmp), &n, NULL) || n == 0) break;
        if (used + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *nb;
            while (ncap < used + n + 1) ncap *= 2;
            nb = (char *)HeapReAlloc(GetProcessHeap(), 0, buf, ncap);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + used, tmp, n);
        used += n;
    }
    if (code != 0xFFFE) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);

    if (buf && used > 0) {
        UINT cp = CP_UTF8;
        int wlen;
        /* 编译器输出通常是 UTF-8；若夹带系统 ANSI 字节（如旧路径）则整体按 ANSI 解码 */
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)used, NULL, 0) <= 0)
            cp = CP_ACP;
        wlen = MultiByteToWideChar(cp, 0, buf, (int)used, NULL, 0);
        WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(WCHAR));
        if (w) {
            MultiByteToWideChar(cp, 0, buf, (int)used, w, wlen);
            w[wlen] = 0;
            *outUtf16 = w;
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    return code;
}

/* ---------------- 文件读写（UTF-8） ---------------- */

static BOOL fileRead(const WCHAR *path, WCHAR **outW) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    char *buf;
    int wlen;
    WCHAR *w;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(h, NULL);
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, buf, size, &got, NULL)) {
        HeapFree(GetProcessHeap(), 0, buf);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    buf[got] = 0;
    /* 跳过 UTF-8 BOM */
    {
        const char *p = buf;
        if (got >= 3 && (unsigned char)buf[0] == 0xEF &&
            (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            p = buf + 3;
        wlen = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
        w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, wlen * sizeof(WCHAR));
        if (!w) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }
        MultiByteToWideChar(CP_UTF8, 0, p, -1, w, wlen);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    *outW = w;
    return TRUE;
}

static BOOL fileWrite(const WCHAR *path, const WCHAR *text) {
    int blen = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, blen);
    HANDLE h;
    DWORD put = 0;
    BOOL ok;
    if (!buf) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, blen, NULL, NULL);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }
    ok = WriteFile(h, buf, blen - 1, &put, NULL);   /* 不含结尾 NUL，无 BOM */
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

/* 取编辑区全文（UTF-16，调用方 HeapFree） */
static WCHAR *editGetAll(void) {
    GETTEXTLENGTHEX gtl;
    GETTEXTEX gt;
    DWORD len;
    WCHAR *t;
    memset(&gtl, 0, sizeof(gtl));
    gtl.flags = GTL_DEFAULT;
    gtl.codepage = 1200;
    len = (DWORD)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    t = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 2) * sizeof(WCHAR));
    if (!t) return NULL;
    memset(&gt, 0, sizeof(gt));
    gt.cb = (len + 1) * sizeof(WCHAR);
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    SendMessageW(g_edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)t);
    t[len] = 0;
    return t;
}

static void editSetAll(const WCHAR *text) {
    SETTEXTEX st;
    memset(&st, 0, sizeof(st));
    st.flags = ST_UNICODE;
    st.codepage = 1200;
    g_loading = TRUE;
    SendMessageW(g_edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)text);
    g_loading = FALSE;
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SendMessageW(g_edit, EM_EMPTYUNDOBUFFER, 0, 0);
}

/* ---------------- 文件命令 ---------------- */

static BOOL confirmDiscard(void) {
    if (!g_modified) return TRUE;
    switch (MessageBoxW(g_hwnd, L"当前文件已修改，是否保存？",
                        L"汉语言编辑器", MB_YESNOCANCEL | MB_ICONQUESTION)) {
    case IDYES: SendMessageW(g_hwnd, WM_COMMAND, IDM_SAVE, 0); return !g_modified;
    case IDNO:  return TRUE;
    default:    return FALSE;
    }
}

static void doNew(void) {
    if (!confirmDiscard()) return;
    g_filePath[0] = 0;
    editSetAll(L"打印 \"你好，汉语言！\"\n");
    g_modified = FALSE;
    updateTitle();
    highlight();
    updateStatus();
}

static BOOL doOpenDialog(WCHAR *path, DWORD n, BOOL save) {
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"汉语言源文件 (*.汉)\0*.汉\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = n;
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                     : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    ofn.lpstrDefExt = L"汉";
    path[0] = 0;
    return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
}

static void doOpenFile(const WCHAR *path) {
    WCHAR *text = NULL;
    if (!fileRead(path, &text)) {
        MessageBoxW(g_hwnd, L"无法读取文件（不存在或编码不是 UTF-8）",
                    L"打开失败", MB_ICONERROR);
        return;
    }
    wcscpy(g_filePath, path);
    editSetAll(text);
    HeapFree(GetProcessHeap(), 0, text);
    g_modified = FALSE;
    updateTitle();
    highlight();
    updateStatus();
    outSetText(L"");
}

static void doOpen(void) {
    WCHAR path[MAX_PATH];
    if (!confirmDiscard()) return;
    if (doOpenDialog(path, MAX_PATH, FALSE)) doOpenFile(path);
}

static BOOL doSaveAs(void) {
    WCHAR path[MAX_PATH];
    if (!doOpenDialog(path, MAX_PATH, TRUE)) return FALSE;
    wcscpy(g_filePath, path);
    return TRUE;
}

static BOOL doSave(void) {
    WCHAR *text;
    BOOL ok;
    if (!g_filePath[0] && !doSaveAs()) return FALSE;
    text = editGetAll();
    if (!text) return FALSE;
    ok = fileWrite(g_filePath, text);
    HeapFree(GetProcessHeap(), 0, text);
    if (!ok) {
        MessageBoxW(g_hwnd, L"写入文件失败", L"保存失败", MB_ICONERROR);
        return FALSE;
    }
    g_modified = FALSE;
    updateTitle();
    return TRUE;
}

/* ---------------- 编译与运行 ---------------- */

/* 取 hanedit.exe 所在目录 */
static void exeDir(WCHAR *dir) {
    WCHAR path[MAX_PATH];
    WCHAR *p;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    p = wcsrchr(path, L'\\');
    if (p) *p = 0;
    wcscpy(dir, path);
}

/* ---------------- 自动定位 han.exe ----------------
 * 依次尝试：同目录 → 上次记住的位置 → PATH 各目录 → 常见安装位置 → 手动选择。
 * 找到后记到 %APPDATA%\汉语言\compiler.ini，下次直接复用。
 * 这样源文件保存在任意位置、编辑器被单独拷走，也能找到编译器。
 */
static BOOL fileExistsW(const WCHAR *p) {
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void hanIniPath(WCHAR *out, int n) {
    WCHAR appdata[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    _snwprintf(out, n, L"%s\\汉语言\\compiler.ini", appdata);
}

static BOOL loadHanPath(WCHAR *out, int n) {
    WCHAR ip[MAX_PATH];
    char buf[MAX_PATH * 2];
    HANDLE h;
    DWORD rd = 0;
    hanIniPath(ip, MAX_PATH);
    h = CreateFileW(ip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL)) { CloseHandle(h); return FALSE; }
    CloseHandle(h);
    buf[rd] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, n) <= 0) return FALSE;
    return fileExistsW(out);
}

static void saveHanPath(const WCHAR *p) {
    WCHAR ip[MAX_PATH], dir[MAX_PATH], *s;
    char buf[MAX_PATH * 2];
    HANDLE h;
    DWORD wr = 0;
    int n;
    hanIniPath(ip, MAX_PATH);
    wcscpy(dir, ip);
    s = wcsrchr(dir, L'\\');
    if (s) { *s = 0; CreateDirectoryW(dir, NULL); }
    n = WideCharToMultiByte(CP_UTF8, 0, p, -1, buf, sizeof(buf), NULL, NULL);
    if (n <= 0) return;
    h = CreateFileW(ip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, (DWORD)(n - 1), &wr, NULL);
    CloseHandle(h);
}

static BOOL tryDir(const WCHAR *dir, WCHAR *out, int n) {
    WCHAR p[MAX_PATH];
    (void)n;
    if (!dir || !dir[0]) return FALSE;
    _snwprintf(p, MAX_PATH, L"%s\\han.exe", dir);
    if (!fileExistsW(p)) return FALSE;
    wcscpy(out, p);
    return TRUE;
}

static BOOL findInPath(WCHAR *out, int n) {
    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
    WCHAR *buf, *tok, *ctx = NULL;
    BOOL ok = FALSE;
    if (need == 0) return FALSE;
    buf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(WCHAR));
    if (!buf) return FALSE;
    if (GetEnvironmentVariableW(L"PATH", buf, need)) {
        for (tok = wcstok(buf, L";", &ctx); tok; tok = wcstok(NULL, L";", &ctx)) {
            if (tryDir(tok, out, n)) { ok = TRUE; break; }
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

/* 安装包会写 HKCU\Software\汉语言\InstallDir，据此定位（用户装到自定义目录也有效） */
static BOOL findInRegistry(WCHAR *out, int n) {
    HKEY k;
    WCHAR dir[MAX_PATH];
    DWORD type = 0, cb = (DWORD)sizeof(dir);
    BOOL ok = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\汉语言", 0, KEY_READ, &k)
        == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"InstallDir", NULL, &type, (BYTE *)dir, &cb)
            == ERROR_SUCCESS && type == REG_SZ)
            ok = tryDir(dir, out, n);
        RegCloseKey(k);
    }
    if (ok) return TRUE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\汉语言", 0, KEY_READ, &k)
        == ERROR_SUCCESS) {
        cb = (DWORD)sizeof(dir);
        type = 0;
        if (RegQueryValueExW(k, L"InstallDir", NULL, &type, (BYTE *)dir, &cb)
            == ERROR_SUCCESS && type == REG_SZ)
            ok = tryDir(dir, out, n);
        RegCloseKey(k);
    }
    return ok;
}

static BOOL findInCommon(WCHAR *out, int n) {
    WCHAR base[MAX_PATH], p[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", base, MAX_PATH)) {
        _snwprintf(p, MAX_PATH, L"%s\\汉语言", base);
        if (tryDir(p, out, n)) return TRUE;
    }
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", base, MAX_PATH)) {
        _snwprintf(p, MAX_PATH, L"%s\\汉语言", base);
        if (tryDir(p, out, n)) return TRUE;
    }
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        _snwprintf(p, MAX_PATH, L"%s\\Programs\\汉语言", base);
        if (tryDir(p, out, n)) return TRUE;
    }
    return FALSE;
}

static BOOL chooseHan(WCHAR *out, int n) {
    OPENFILENAMEW ofn;
    WCHAR file[MAX_PATH] = L"";
    (void)n;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"汉语言编译器 (han.exe)\0han.exe\0可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"请选择 han.exe 编译器";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return FALSE;
    wcscpy(out, file);
    return TRUE;
}

/* 定位编译器；ask=TRUE 时找不到会弹出选择框 */
static BOOL findHan(WCHAR *out, int n, BOOL ask) {
    WCHAR dir[MAX_PATH];
    if (g_hanPath[0] && fileExistsW(g_hanPath)) { wcscpy(out, g_hanPath); return TRUE; }
    exeDir(dir);
    if (tryDir(dir, out, n)) goto found;               /* 1 编辑器同目录 */
    if (loadHanPath(out, n)) goto found;               /* 2 上次记住的位置 */
    if (findInPath(out, n)) goto found;                /* 3 PATH */
    if (findInRegistry(out, n)) goto found;            /* 4 安装包登记的安装目录 */
    if (findInCommon(out, n)) goto found;              /* 5 常见安装位置 */
    if (ask && chooseHan(out, n)) goto found;          /* 6 手动选择 */
    return FALSE;
found:
    wcscpy(g_hanPath, out);
    saveHanPath(out);
    return TRUE;
}

/* 在独立控制台窗口中运行程序（不等待返回）。
   窗口由系统创建，程序可正常键盘输入；结束时运行时自动暂停等待回车。 */
static BOOL run_visible(const WCHAR *exe) {
    WCHAR cmd[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    _snwprintf(cmd, MAX_PATH + 4, L"\"%s\"", exe);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
    if (!ok) return FALSE;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

static void doBuild(BOOL run) {
    WCHAR han[MAX_PATH], outExe[MAX_PATH], cmd[MAX_PATH * 3 + 64];
    WCHAR *txt = NULL, *p;
    DWORD code;

    if (!g_filePath[0]) {
        if (!doSaveAs()) return;
    }
    if (g_modified && !doSave()) return;

    if (!findHan(han, MAX_PATH, TRUE)) {
        MessageBoxW(g_hwnd,
                    L"未找到 han.exe（编译器）。\n"
                    L"请点「帮助 → 设置编译器位置…」手动指定，或重新安装汉语言。",
                    L"无法编译", MB_ICONERROR);
        return;
    }

    /* 输出 exe：源文件同目录同名 .exe */
    wcscpy(outExe, g_filePath);
    p = wcsrchr(outExe, L'.');
    if (p) *p = 0;
    wcscat(outExe, L".exe");

    outSetText(L"");
    outAppend(L"══ 编译 ══\r\n");
    _snwprintf(cmd, MAX_PATH * 3 + 64, L"\"%s\" \"%s\" -o \"%s\"", han, g_filePath, outExe);
    code = run_capture(cmd, &txt);
    if (txt) { outAppend(txt); HeapFree(GetProcessHeap(), 0, txt); txt = NULL; }

    if (code == 0xFFFF) {
        WCHAR msg[MAX_PATH * 2];
        _snwprintf(msg, MAX_PATH * 2,
                   L"✗ 无法启动编译器：%s\r\n"
                   L"  请检查文件是否存在、是否有执行权限，或用「帮助 → 设置编译器位置…」重新指定。\r\n",
                   han);
        outAppend(msg);
        outColorize();
        return;
    }
    if (code == 0xFFFE) {
        outAppend(L"✗ 编译超时，已终止编译器进程\r\n");
        outColorize();
        return;
    }
    if (code != 0) {
        WCHAR msg[160];
        _snwprintf(msg, 160, L"✗ 编译失败（退出码 %lu），双击上方错误行可跳转到源码\r\n",
                   (unsigned long)code);
        outAppend(msg);
        outColorize();
        return;
    }
    outAppend(L"✓ 编译成功\r\n");

    if (run) {
        /* 在独立控制台窗口中运行：可键盘输入，程序结束自动暂停 */
        if (run_visible(outExe))
            outAppend(L"已在独立窗口运行（可键盘输入，程序结束自动暂停）\r\n");
        else
            outAppend(L"✗ 启动运行窗口失败\r\n");
    }
    outColorize();
}

static void doCheck(void) {
    WCHAR han[MAX_PATH], cmd[MAX_PATH * 2 + 64];
    WCHAR *txt = NULL;
    DWORD code;

    if (!g_filePath[0]) {
        if (!doSaveAs()) return;
    }
    if (g_modified && !doSave()) return;

    if (!findHan(han, MAX_PATH, TRUE)) {
        MessageBoxW(g_hwnd,
                    L"未找到 han.exe（编译器）。\n"
                    L"请点「帮助 → 设置编译器位置…」手动指定，或重新安装汉语言。",
                    L"无法检查", MB_ICONERROR);
        return;
    }
    outSetText(L"");
    outAppend(L"══ 语法与语义检查 ══\r\n");
    _snwprintf(cmd, MAX_PATH * 2 + 64, L"\"%s\" --check \"%s\"", han, g_filePath);
    code = run_capture(cmd, &txt);
    if (txt) { outAppend(txt); HeapFree(GetProcessHeap(), 0, txt); }
    outAppend(code == 0 ? L"✓ 检查通过\r\n"
                        : L"✗ 检查未通过（双击上方错误行可跳转到源码）\r\n");
    outColorize();
}

/* 帮助菜单：手动指定 / 更换编译器位置 */
static void doLocateCompiler(void) {
    WCHAR p[MAX_PATH];
    WCHAR msg[MAX_PATH * 2 + 64];
    if (findHan(p, MAX_PATH, FALSE)) {
        _snwprintf(msg, MAX_PATH * 2 + 64, L"当前编译器：\n%s\n\n是否重新指定？", p);
        if (MessageBoxW(g_hwnd, msg, L"编译器位置",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    if (chooseHan(p, MAX_PATH)) {
        wcscpy(g_hanPath, p);
        saveHanPath(p);
        _snwprintf(msg, MAX_PATH * 2 + 64, L"已记住编译器位置：\n%s", p);
        MessageBoxW(g_hwnd, msg, L"设置成功", MB_ICONINFORMATION);
    }
}

/* 输出面板双击：解析「文件.汉:行:列」并跳转编辑区对应行 */
static void jumpToErrorLine(void) {
    CHARRANGE cr;
    DWORD lineNo, start, llen, n;
    WCHAR buf[512];
    WORD wn;
    WCHAR *hit, *p;
    long row, col, pos;

    SendMessageW(g_out, EM_EXGETSEL, 0, (LPARAM)&cr);
    lineNo = (DWORD)SendMessageW(g_out, EM_EXLINEFROMCHAR, (WPARAM)-1, (LPARAM)cr.cpMin);
    start = (DWORD)SendMessageW(g_out, EM_LINEINDEX, (WPARAM)lineNo, 0);
    llen = (DWORD)SendMessageW(g_out, EM_LINELENGTH, (WPARAM)start, 0);
    if (llen == 0 || llen >= 510) return;
    n = llen;
    wn = (WORD)n;
    memcpy(buf, &wn, sizeof(WORD));
    SendMessageW(g_out, EM_GETLINE, (WPARAM)lineNo, (LPARAM)buf);
    buf[n] = 0;

    hit = wcsstr(buf, L".汉:");
    if (!hit) return;
    p = hit + 3;
    row = wcstol(p, &p, 10);
    if (*p != L':') return;
    col = wcstol(p + 1, NULL, 10);
    if (row < 1) return;

    pos = (long)SendMessageW(g_edit, EM_LINEINDEX, (WPARAM)(row - 1), 0);
    if (pos < 0) pos = 0;
    if (col > 1) pos += col - 1;
    cr.cpMin = cr.cpMax = pos;
    SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SetFocus(g_edit);
    updateStatus();
}

/* ---------------- 帮助 ---------------- */

/* 取文件所在目录 */
static void dirOf(const WCHAR *file, WCHAR *out) {
    WCHAR *s;
    wcscpy(out, file);
    s = wcsrchr(out, L'\\');
    if (s) s[0] = 0;
    else out[0] = 0;
}

/* 依次尝试候选路径，打开第一个存在的文件 */
static BOOL openFirst(const WCHAR *const *paths, int n) {
    for (int i = 0; i < n; i++) {
        if (GetFileAttributesW(paths[i]) == INVALID_FILE_ATTRIBUTES) continue;
        if ((INT_PTR)ShellExecuteW(g_hwnd, L"open", paths[i], NULL, NULL, SW_SHOW) > 32)
            return TRUE;
    }
    return FALSE;
}

/* 教程可能的位置：安装版 {app}\doc、绿色版 build\..\doc、与编译器同装的目录 */
static int docCandidates(const WCHAR *name, WCHAR paths[][MAX_PATH], const WCHAR **list) {
    WCHAR dir[MAX_PATH], hanDir[MAX_PATH];
    int n = 0;
    exeDir(dir);
    _snwprintf(paths[n], MAX_PATH, L"%s\\doc\\%s", dir, name);       list[n] = paths[n]; n++;
    _snwprintf(paths[n], MAX_PATH, L"%s\\..\\doc\\%s", dir, name);    list[n] = paths[n]; n++;
    _snwprintf(paths[n], MAX_PATH, L"%s\\%s", dir, name);             list[n] = paths[n]; n++;
    if (g_hanPath[0]) {
        dirOf(g_hanPath, hanDir);
        _snwprintf(paths[n], MAX_PATH, L"%s\\doc\\%s", hanDir, name); list[n] = paths[n]; n++;
    }
    return n;
}

static void doHelp(void) {
    WCHAR paths[8][MAX_PATH];
    const WCHAR *list[8];
    int n = docCandidates(L"教程\\教程.html", paths, list);
    if (openFirst(list, n)) return;
    n = docCandidates(L"教程.md", paths, list);
    if (openFirst(list, n)) return;
    MessageBoxW(g_hwnd,
                L"未找到教程文件。\n"
                L"正常安装后教程位于「安装目录\\doc\\教程\\教程.html」；\n"
                L"也可打开开始菜单中的「汉语言教程」。",
                L"帮助", MB_ICONINFORMATION);
}

/* 打开使用手册（Markdown） */
static void doManual(void) {
    WCHAR paths[8][MAX_PATH];
    const WCHAR *list[8];
    int n = docCandidates(L"使用手册.md", paths, list);
    if (openFirst(list, n)) return;
    MessageBoxW(g_hwnd, L"未找到使用手册（doc\\使用手册.md）。",
                L"帮助", MB_ICONINFORMATION);
}

static void doAbout(void) {
    MessageBoxW(g_hwnd,
                L"汉语言编辑器 hanedit 1.0\n\n"
                L"纯 C + Win32 自研，无第三方依赖。\n"
                L"F5 编译运行　F6 语法检查　F1 教程\n"
                L"双击输出面板错误行可跳转到源码位置。",
                L"关于", MB_ICONINFORMATION);
}

/* ---------------- 布局 ---------------- */

static void layout(void) {
    RECT rc;
    int sbH = 0, editH;
    RECT sr;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    GetWindowRect(g_status, &sr);
    sbH = sr.bottom - sr.top;
    if (g_outHeight < OUT_MIN) g_outHeight = OUT_MIN;
    if (g_outHeight > rc.bottom - sbH - 80) g_outHeight = rc.bottom - sbH - 80;
    editH = rc.bottom - sbH - g_outHeight - SPLIT_H;
    if (editH < 40) editH = 40;
    MoveWindow(g_edit, 0, 0, rc.right, editH, TRUE);
    MoveWindow(g_out, 0, editH + SPLIT_H, rc.right, g_outHeight, TRUE);
}

static BOOL onSplitArea(POINT pt) {
    RECT rc;
    int sbH, editH;
    RECT sr;
    GetClientRect(g_hwnd, &rc);
    GetWindowRect(g_status, &sr);
    sbH = sr.bottom - sr.top;
    editH = rc.bottom - sbH - g_outHeight - SPLIT_H;
    return pt.y >= editH && pt.y <= editH + SPLIT_H + 2;
}

/* ---------------- 主窗口过程 ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if ((g_dragging || onSplitArea(pt)) && (HWND)wp == hwnd) {
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (onSplitArea(pt)) {
            g_dragging = TRUE;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT rc, sr;
            int y = (short)HIWORD(lp);
            GetClientRect(hwnd, &rc);
            GetWindowRect(g_status, &sr);
            g_outHeight = rc.bottom - (sr.bottom - sr.top) - SPLIT_H - y;
            if (g_outHeight < OUT_MIN) g_outHeight = OUT_MIN;
            layout();
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_HIGHLIGHT) {
            KillTimer(hwnd, IDT_HIGHLIGHT);
            highlight();
        }
        return 0;

    case WM_DROPFILES: {
        WCHAR path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileW(drop, 0, path, MAX_PATH) && confirmDiscard())
            doOpenFile(path);
        DragFinish(drop);
        return 0;
    }

    case WM_COMMAND:
        switch (HIWORD(wp)) {
        case EN_CHANGE:
            if ((HWND)lp == g_edit && !g_loading) {
                if (!g_modified) { g_modified = TRUE; updateTitle(); }
                SetTimer(hwnd, IDT_HIGHLIGHT, 250, NULL);
            }
            return 0;
        default:
            break;
        }
        switch (LOWORD(wp)) {
        case IDM_NEW:    doNew(); return 0;
        case IDM_OPEN:   doOpen(); return 0;
        case IDM_SAVE:   doSave(); return 0;
        case IDM_SAVEAS: if (doSaveAs()) doSave(); return 0;
        case IDM_EXIT:   SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_UNDO:   SendMessageW(g_edit, WM_UNDO, 0, 0); return 0;
        case IDM_REDO:   SendMessageW(g_edit, EM_REDO, 0, 0); return 0;
        case IDM_CUT:    SendMessageW(g_edit, WM_CUT, 0, 0); return 0;
        case IDM_COPY:   SendMessageW(g_edit, WM_COPY, 0, 0); return 0;
        case IDM_PASTE:  SendMessageW(g_edit, WM_PASTE, 0, 0); return 0;
        case IDM_SELALL: {
            CHARRANGE all = { 0, -1 };
            SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&all);
            return 0;
        }
        case IDM_RUN:    doBuild(TRUE); return 0;
        case IDM_CHECK:  doCheck(); return 0;
        case IDM_HELP:   doHelp(); return 0;
        case IDM_MANUAL: doManual(); return 0;
        case IDM_LOCATE: doLocateCompiler(); return 0;
        case IDM_ABOUT:  doAbout(); return 0;
        case ID_EDIT:
            if (HIWORD(wp) == EN_SELCHANGE || HIWORD(wp) == EN_CHANGE)
                updateStatus();
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->hwndFrom == g_edit && nh->code == EN_SELCHANGE) {
            updateStatus();
            return 0;
        }
        if (nh->hwndFrom == g_out && nh->code == EN_MSGFILTER) {
            MSGFILTER *mf = (MSGFILTER *)lp;
            if (mf->msg == WM_LBUTTONDBLCLK) jumpToErrorLine();
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (confirmDiscard()) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 菜单与快捷键 ---------------- */

static HMENU buildMenu(void) {
    HMENU bar = CreateMenu();
    HMENU m;

    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_NEW,    L"新建(&N)\tCtrl+N");
    AppendMenuW(m, MF_STRING, IDM_OPEN,   L"打开(&O)…\tCtrl+O");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SAVE,   L"保存(&S)\tCtrl+S");
    AppendMenuW(m, MF_STRING, IDM_SAVEAS, L"另存为(&A)…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT,   L"退出(&X)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"文件(&F)");

    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_UNDO,   L"撤销(&U)\tCtrl+Z");
    AppendMenuW(m, MF_STRING, IDM_REDO,   L"重做(&R)\tCtrl+Y");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_CUT,    L"剪切(&T)\tCtrl+X");
    AppendMenuW(m, MF_STRING, IDM_COPY,   L"复制(&C)\tCtrl+C");
    AppendMenuW(m, MF_STRING, IDM_PASTE,  L"粘贴(&P)\tCtrl+V");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SELALL, L"全选(&A)\tCtrl+A");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"编辑(&E)");

    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_RUN,   L"编译并运行(&R)\tF5");
    AppendMenuW(m, MF_STRING, IDM_CHECK, L"仅语法检查(&C)\tF6");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"构建(&B)");

    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_HELP,  L"打开教程(&H)\tF1");
    AppendMenuW(m, MF_STRING, IDM_MANUAL, L"打开使用手册(&M)");
    AppendMenuW(m, MF_STRING, IDM_LOCATE, L"设置编译器位置(&L)…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"关于(&A)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"帮助(&H)");

    return bar;
}

static HACCEL buildAccel(void) {
    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_SAVE },
        { FVIRTKEY | FCONTROL, 'Z', IDM_UNDO },
        { FVIRTKEY | FCONTROL, 'Y', IDM_REDO },
        { FVIRTKEY, VK_F5, IDM_RUN },
        { FVIRTKEY, VK_F6, IDM_CHECK },
        { FVIRTKEY, VK_F1, IDM_HELP },
    };
    return CreateAcceleratorTableW(acc, sizeof(acc) / sizeof(acc[0]));
}

/* 设置控件默认字体（Consolas，中文自动回退系统字体） */
static void setMonoFont(HWND ctl, int pointSize) {
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE;
    cf.yHeight = pointSize * 20;
    wcscpy(cf.szFaceName, L"Consolas");
    SendMessageW(ctl, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

/* ---------------- 入口 ---------------- */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdlineA, int show) {
    WNDCLASSW wc;
    MSG msg;
    HACCEL accel;
    HMODULE hRich;
    INITCOMMONCONTROLSEX icc;
    int argc = 0;
    LPWSTR *argv;

    (void)prev;
    (void)cmdlineA;

    hRich = LoadLibraryW(L"Msftedit.dll");
    if (!hRich) {
        MessageBoxW(NULL, L"无法加载 Msftedit.dll（RichEdit 4.1）",
                    L"启动失败", MB_ICONERROR);
        return 1;
    }
    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(245, 246, 248));
    wc.lpszClassName = L"HanEditMain";
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"汉语言编辑器",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700,
                             NULL, buildMenu(), inst, NULL);
    if (!g_hwnd) return 1;

    /* 编辑区 */
    g_edit = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                             ES_NOHIDESEL,
                             0, 0, 100, 100, g_hwnd, (HMENU)ID_EDIT, inst, NULL);
    /* 输出面板（只读） */
    g_out = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                            ES_NOHIDESEL,
                            0, 0, 100, 100, g_hwnd, (HMENU)ID_OUT, inst, NULL);
    /* 状态栏（扁平化：无大小手柄） */
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                               WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, g_hwnd, (HMENU)ID_STATUS, inst, NULL);

    SendMessageW(g_edit, EM_SETEVENTMASK, 0,
                 ENM_CHANGE | ENM_SELCHANGE | ENM_KEYEVENTS);
    SendMessageW(g_out, EM_SETEVENTMASK, 0, ENM_MOUSEEVENTS);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    setMonoFont(g_edit, 12);
    setMonoFont(g_out, 11);
    /* 编辑区左右留一点呼吸空间 */
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    SendMessageW(g_out, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    /*
     * 状态栏分三段：行列 / 编码 / 编译器状态
     */
    {
        int parts[3] = { 240, 380, -1 };
        SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
    }
    SendMessageW(g_status, SB_SETBKCOLOR, 0, (LPARAM)RGB(245, 246, 248));
    /* 任务栏 / 标题栏图标 */
    {
        HICON hi = LoadIcon(inst, MAKEINTRESOURCEW(1));
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hi);
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hi);
    }
    /* 启动时先静默定位一次编译器，状态栏可见 */
    {
        WCHAR hanp[MAX_PATH];
        findHan(hanp, MAX_PATH, FALSE);
    }

    editSetAll(L"打印 \"你好，汉语言！\"\n");
    layout();
    updateTitle();
    highlight();
    updateStatus();

    /* 命令行参数：hanedit.exe 文件.汉 直接打开 */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) doOpenFile(argv[1]);
    if (argv) LocalFree(argv);

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    accel = buildAccel();
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorW(g_hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accel);
    return (int)msg.wParam;
}
