/*
 * Port Scanner GUI — Windows native, no dependencies
 * Compile: x86_64-w64-mingw32-g++ -std=c++17 -O2 -o PortScanner.exe port_scanner_gui.cpp
 *          -lws2_32 -lcomctl32 -mwindows
 */

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows")

// ── IDs ──────────────────────────────────────────────────────────────────
#define ID_HOST        101
#define ID_START_PORT  102
#define ID_END_PORT    103
#define ID_THREADS     104
#define ID_TIMEOUT     105
#define ID_SCAN_BTN    106
#define ID_STOP_BTN    107
#define ID_CLEAR_BTN   108
#define ID_LISTVIEW    109
#define ID_PROGRESS    110
#define ID_STATUS      111
#define ID_PRESET_COMMON 112
#define ID_PRESET_FULL   113
#define ID_PRESET_WEB    114
#define WM_SCAN_RESULT (WM_USER + 1)
#define WM_SCAN_DONE   (WM_USER + 2)
#define WM_SCAN_PROG   (WM_USER + 3)

// ── Port name map ────────────────────────────────────────────────────────
static const std::map<UINT16, const wchar_t*> kPortNames = {
    {20,L"FTP-Data"},{21,L"FTP"},{22,L"SSH"},{23,L"Telnet"},{25,L"SMTP"},
    {53,L"DNS"},{67,L"DHCP"},{80,L"HTTP"},{110,L"POP3"},{123,L"NTP"},
    {135,L"MS-RPC"},{139,L"NetBIOS"},{143,L"IMAP"},{161,L"SNMP"},{194,L"IRC"},
    {389,L"LDAP"},{443,L"HTTPS"},{445,L"SMB"},{465,L"SMTPS"},{587,L"SMTP-Sub"},
    {636,L"LDAPS"},{993,L"IMAPS"},{995,L"POP3S"},{1080,L"SOCKS"},{1194,L"OpenVPN"},
    {1433,L"MSSQL"},{1521,L"Oracle"},{1723,L"PPTP"},{2049,L"NFS"},{3306,L"MySQL"},
    {3389,L"RDP"},{5000,L"Flask/UPnP"},{5432,L"PostgreSQL"},{5672,L"AMQP"},
    {5900,L"VNC"},{6379,L"Redis"},{8080,L"HTTP-Alt"},{8443,L"HTTPS-Alt"},
    {8888,L"Jupyter"},{9200,L"Elasticsearch"},{11211,L"Memcached"},
    {27017,L"MongoDB"},{3389,L"RDP"},
};

const wchar_t* PortName(UINT16 p) {
    auto it = kPortNames.find(p);
    return it != kPortNames.end() ? it->second : L"unknown";
}

// ── Global state ─────────────────────────────────────────────────────────
HWND  g_hWnd = nullptr;
HWND  g_hList, g_hProgress, g_hStatus;
HWND  g_hHost, g_hStartPort, g_hEndPort, g_hThreads, g_hTimeout;
HWND  g_hScanBtn, g_hStopBtn;

std::atomic<bool>     g_running{false};
std::atomic<UINT32>   g_scanned{0};
std::atomic<UINT32>   g_total{0};
std::atomic<int>      g_openCount{0};
std::mutex            g_qMtx;
std::queue<UINT16>    g_workQ;

struct ScanResult { UINT16 port; std::wstring service; std::wstring state; };

// ── Helpers ──────────────────────────────────────────────────────────────
std::string resolveHost(const std::string& host) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return {};
    char buf[INET_ADDRSTRLEN] = {};
    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

bool tcpProbe(const std::string& ip, UINT16 port, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
    timeval tv{ timeout_ms/1000, (timeout_ms%1000)*1000 };
    bool open = false;
    if (select(0, nullptr, &wfds, nullptr, &tv) > 0) {
        int err = 0; int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        open = (err == 0);
    }
    closesocket(s);
    return open;
}

// ── Worker thread ─────────────────────────────────────────────────────────
void workerThread(std::string ip, int timeout_ms) {
    while (g_running) {
        UINT16 port;
        {
            std::lock_guard<std::mutex> lk(g_qMtx);
            if (g_workQ.empty()) break;
            port = g_workQ.front(); g_workQ.pop();
        }
        if (tcpProbe(ip, port, timeout_ms)) {
            auto* r = new ScanResult{port, PortName(port), L"open"};
            PostMessage(g_hWnd, WM_SCAN_RESULT, 0, (LPARAM)r);
            g_openCount++;
        }
        UINT32 done = ++g_scanned;
        // throttle progress messages
        if (done % 5 == 0 || done == g_total)
            PostMessage(g_hWnd, WM_SCAN_PROG, done, g_total);
    }
}

// ── List view helpers ─────────────────────────────────────────────────────
void LV_AddResult(const ScanResult& r) {
    LVITEM lvi{};
    lvi.mask = LVIF_TEXT;
    int idx = ListView_GetItemCount(g_hList);
    lvi.iItem = idx;

    wchar_t ps[16];
    swprintf_s(ps, L"%u/tcp", r.port);
    lvi.pszText = ps;
    lvi.iSubItem = 0;
    ListView_InsertItem(g_hList, &lvi);

    ListView_SetItemText(g_hList, idx, 1, (LPWSTR)r.service.c_str());
    ListView_SetItemText(g_hList, idx, 2, (LPWSTR)r.state.c_str());
}

void LV_Init(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMN lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    lvc.cx = 100; lvc.pszText = (LPWSTR)L"Port";      lvc.iSubItem=0; ListView_InsertColumn(hList,0,&lvc);
    lvc.cx = 170; lvc.pszText = (LPWSTR)L"Service";   lvc.iSubItem=1; ListView_InsertColumn(hList,1,&lvc);
    lvc.cx = 80;  lvc.pszText = (LPWSTR)L"State";     lvc.iSubItem=2; ListView_InsertColumn(hList,2,&lvc);
}

// ── UI helpers ───────────────────────────────────────────────────────────
HWND MakeLabel(HWND par, const wchar_t* txt, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", txt, WS_CHILD|WS_VISIBLE|SS_LEFT,
                         x, y, w, h, par, nullptr, GetModuleHandle(nullptr), nullptr);
}
HWND MakeEdit(HWND par, const wchar_t* txt, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowW(L"EDIT", txt, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
                            x, y, w, h, par, (HMENU)(UINT_PTR)id,
                            GetModuleHandle(nullptr), nullptr);
    return hw;
}
HWND MakeButton(HWND par, const wchar_t* txt, int x, int y, int w, int h, int id) {
    return CreateWindowW(L"BUTTON", txt, WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                         x, y, w, h, par, (HMENU)(UINT_PTR)id,
                         GetModuleHandle(nullptr), nullptr);
}

// ── Scan launcher ─────────────────────────────────────────────────────────
void StartScan() {
    if (g_running) return;

    wchar_t buf[256];
    GetWindowTextW(g_hHost, buf, 256);
    std::wstring whost(buf);
    if (whost.empty()) { MessageBoxW(g_hWnd, L"Enter a host!", L"Error", MB_OK|MB_ICONWARNING); return; }
    std::string host(whost.begin(), whost.end());

    wchar_t tmp[32];
    GetWindowTextW(g_hStartPort, tmp, 32); int sp = _wtoi(tmp);
    GetWindowTextW(g_hEndPort,   tmp, 32); int ep = _wtoi(tmp);
    GetWindowTextW(g_hThreads,   tmp, 32); int th = _wtoi(tmp);
    GetWindowTextW(g_hTimeout,   tmp, 32); int to = _wtoi(tmp);
    if (sp < 1 || ep > 65535 || sp > ep) { MessageBoxW(g_hWnd,L"Invalid port range.",L"Error",MB_OK|MB_ICONWARNING); return; }
    if (th < 1 || th > 1024) th = 128;
    if (to < 50 || to > 30000) to = 1000;

    std::string ip = resolveHost(host);
    if (ip.empty()) { MessageBoxW(g_hWnd,L"Cannot resolve host.",L"Error",MB_OK|MB_ICONERROR); return; }

    // Clear list
    ListView_DeleteAllItems(g_hList);
    g_scanned = 0; g_total = (UINT32)(ep - sp + 1); g_openCount = 0;

    while (!g_workQ.empty()) g_workQ.pop();
    for (int p = sp; p <= ep; ++p) g_workQ.push((UINT16)p);

    SendMessage(g_hProgress, PBM_SETRANGE32, 0, g_total);
    SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

    wchar_t st[256];
    swprintf_s(st, L"Scanning %hs  (%hs)  ports %d–%d  |  threads: %d  timeout: %dms",
               host.c_str(), ip.c_str(), sp, ep, th, to);
    SetWindowTextW(g_hStatus, st);

    EnableWindow(g_hScanBtn, FALSE);
    EnableWindow(g_hStopBtn, TRUE);

    g_running = true;
    // spawn threads + a monitor thread
    std::thread([ip, th, to]() {
        std::vector<std::thread> pool;
        for (int i = 0; i < th; ++i)
            pool.emplace_back(workerThread, ip, to);
        for (auto& t : pool) t.join();
        PostMessage(g_hWnd, WM_SCAN_DONE, 0, 0);
    }).detach();
}

void StopScan() {
    g_running = false;
}

// ── Window Procedure ──────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // ── Left panel labels & edits
        int lx = 14, ly = 12, lh = 20, ew = 170;
        auto setFont = [&](HWND h){ SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE); };

        setFont(MakeLabel(hWnd,L"Host / IP:",    lx, ly,     90, lh));
        g_hHost = MakeEdit(hWnd, L"scanme.nmap.org", lx+95, ly, ew, lh, ID_HOST);
        setFont(g_hHost);

        ly += 28;
        setFont(MakeLabel(hWnd,L"Start port:",  lx, ly,     90, lh));
        g_hStartPort = MakeEdit(hWnd, L"1",    lx+95, ly, 75, lh, ID_START_PORT);
        setFont(g_hStartPort);

        ly += 28;
        setFont(MakeLabel(hWnd,L"End port:",    lx, ly,     90, lh));
        g_hEndPort = MakeEdit(hWnd, L"1024",   lx+95, ly, 75, lh, ID_END_PORT);
        setFont(g_hEndPort);

        ly += 28;
        setFont(MakeLabel(hWnd,L"Threads:",     lx, ly,     90, lh));
        g_hThreads = MakeEdit(hWnd, L"128",    lx+95, ly, 75, lh, ID_THREADS);
        setFont(g_hThreads);

        ly += 28;
        setFont(MakeLabel(hWnd,L"Timeout (ms):",lx, ly,     90, lh));
        g_hTimeout = MakeEdit(hWnd, L"1000",   lx+95, ly, 75, lh, ID_TIMEOUT);
        setFont(g_hTimeout);

        // ── Preset buttons
        ly += 36;
        setFont(MakeLabel(hWnd,L"Presets:",     lx, ly, 90, lh));
        setFont(MakeButton(hWnd,L"Common",   lx,    ly+22, 82, 26, ID_PRESET_COMMON));
        setFont(MakeButton(hWnd,L"Web",      lx+86, ly+22, 60, 26, ID_PRESET_WEB));
        setFont(MakeButton(hWnd,L"Full",     lx+150,ly+22, 60, 26, ID_PRESET_FULL));

        // ── Action buttons
        ly += 72;
        g_hScanBtn = MakeButton(hWnd,L"▶  Scan",  lx,    ly, 100, 32, ID_SCAN_BTN);
        g_hStopBtn = MakeButton(hWnd,L"■  Stop",  lx+108,ly, 100, 32, ID_STOP_BTN);
        setFont(g_hScanBtn); setFont(g_hStopBtn);
        EnableWindow(g_hStopBtn, FALSE);

        HWND hClr = MakeButton(hWnd,L"Clear",     lx+220,ly, 60, 32, ID_CLEAR_BTN);
        setFont(hClr);

        // ── List view (results)
        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS,
            300, 10, 460, 340, hWnd, (HMENU)(UINT_PTR)ID_LISTVIEW,
            GetModuleHandle(nullptr), nullptr);
        LV_Init(g_hList);

        // ── Progress bar
        g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            300, 358, 460, 18, hWnd, (HMENU)(UINT_PTR)ID_PROGRESS,
            GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hProgress, PBM_SETRANGE32, 0, 100);

        // ── Status bar
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready.",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)(UINT_PTR)ID_STATUS,
            GetModuleHandle(nullptr), nullptr);
        setFont(g_hStatus);
        break;
    }

    case WM_SIZE: {
        int W = LOWORD(lp), H = HIWORD(lp);
        // resize list + progress to fill right panel
        SetWindowPos(g_hList,     nullptr, 300, 10,  W-310, H-80,  SWP_NOZORDER);
        SetWindowPos(g_hProgress, nullptr, 300, H-65,W-310, 18,    SWP_NOZORDER);
        SendMessage(g_hStatus, WM_SIZE, 0, 0);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_SCAN_BTN)  { StartScan(); break; }
        if (id == ID_STOP_BTN)  { StopScan(); SetWindowTextW(g_hStatus, L"Stopped."); break; }
        if (id == ID_CLEAR_BTN) {
            ListView_DeleteAllItems(g_hList);
            SendMessage(g_hProgress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_hStatus, L"Cleared.");
            break;
        }
        if (id == ID_PRESET_COMMON) {
            SetWindowTextW(g_hStartPort, L"1");
            SetWindowTextW(g_hEndPort,   L"1024");
            SetWindowTextW(g_hThreads,   L"128");
            SetWindowTextW(g_hTimeout,   L"1000");
            break;
        }
        if (id == ID_PRESET_WEB) {
            SetWindowTextW(g_hStartPort, L"80");
            SetWindowTextW(g_hEndPort,   L"8888");
            SetWindowTextW(g_hThreads,   L"64");
            SetWindowTextW(g_hTimeout,   L"800");
            break;
        }
        if (id == ID_PRESET_FULL) {
            SetWindowTextW(g_hStartPort, L"1");
            SetWindowTextW(g_hEndPort,   L"65535");
            SetWindowTextW(g_hThreads,   L"512");
            SetWindowTextW(g_hTimeout,   L"500");
            break;
        }
        break;
    }

    case WM_SCAN_RESULT: {
        auto* r = reinterpret_cast<ScanResult*>(lp);
        LV_AddResult(*r);
        delete r;
        break;
    }

    case WM_SCAN_PROG: {
        UINT32 done = (UINT32)wp, tot = (UINT32)lp;
        SendMessage(g_hProgress, PBM_SETPOS, done, 0);
        wchar_t st[256];
        swprintf_s(st, L"Scanning...  %u / %u ports  |  %d open",
                   done, tot, g_openCount.load());
        SetWindowTextW(g_hStatus, st);
        break;
    }

    case WM_SCAN_DONE: {
        g_running = false;
        EnableWindow(g_hScanBtn, TRUE);
        EnableWindow(g_hStopBtn, FALSE);
        SendMessage(g_hProgress, PBM_SETPOS, g_total, 0);
        wchar_t st[256];
        swprintf_s(st, L"Done.  Scanned %u ports  |  %d open",
                   g_total.load(), g_openCount.load());
        SetWindowTextW(g_hStatus, st);
        break;
    }

    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
    return 0;
}

// ── WinMain ───────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WSADATA wd{}; WSAStartup(MAKEWORD(2,2), &wd);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS|ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszClassName = L"PortScannerWnd";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"PortScannerWnd", L"Port Scanner",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 790, 450,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return (int)msg.wParam;
}
