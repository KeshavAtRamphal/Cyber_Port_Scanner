# 🔍 C++ Windows GUI Port Scanner
 
A lightweight, multi-threaded TCP port scanner with a native Windows GUI — no dependencies, no install required. Just download and run.
 
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)
![License](https://img.shields.io/badge/license-MIT-green)
 
---
 
## Screenshot
 
> A persistent window with real-time results, preset scan modes, live progress bar, and a clean results table.
 
```
┌────────────────────────────────────────────────┐
│  Host: scanme.nmap.org        [▶ Scan] [■ Stop] │
│  Start: 1   End: 1024   Threads: 128            │
│  ┌──────────┬──────────────────┬────────┐       │
│  │ Port     │ Service          │ State  │       │
│  ├──────────┼──────────────────┼────────┤       │
│  │ 22/tcp   │ SSH              │ open   │       │
│  │ 80/tcp   │ HTTP             │ open   │       │
│  │ 443/tcp  │ HTTPS            │ open   │       │
│  └──────────┴──────────────────┴────────┘       │
│  [############################..] 87.3%          │
│  Scanning... 894 / 1024 ports | 3 open           │
└────────────────────────────────────────────────┘
```
 
---
 
## Features
 
- **Persistent GUI** — native Win32 window, stays open until you close it
- **Multi-threaded scanning** — configurable thread count (default 128) for fast scanning
- **Non-blocking connects** — `select()`-based with per-port timeout; no thread ever hangs
- **Live results table** — open ports appear in real time with port number, service name, and state
- **Progress bar** — shows percentage complete and running open port count
- **Preset modes** — one-click Common, Web, and Full scan configurations
- **Stop / Clear** — cancel mid-scan or wipe results and start fresh
- **Service labels** — recognises ~50 well-known services (SSH, HTTP, MySQL, RDP, Redis, etc.)
- **Zero dependencies** — single `.exe`, no runtimes or installers needed
---
 
## Download & Run
 
1. Go to [**Releases**](../../releases)
2. Download `PortScanner.exe`
3. Double-click to run — no install needed
**Requirements:** Windows 7 or later (64-bit)
 
---
 
## Build from Source
 
### Prerequisites
 
| Tool | Download |
|------|----------|
| MinGW-w64 (GCC for Windows) | https://www.mingw-w64.org/ |
| Or: MSYS2 (recommended) | https://www.msys2.org/ |
 
### Compile
 
**On Windows (MSYS2 / MinGW terminal):**
```bash
g++ -std=c++17 -O2 -o PortScanner.exe port_scanner_gui.cpp -lws2_32 -lcomctl32 -mwindows
```
 
**Cross-compile on Linux (MinGW cross-compiler):**
```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -o PortScanner.exe port_scanner_gui.cpp -lws2_32 -lcomctl32 -mwindows
```
 
The `-mwindows` flag suppresses the console window so only the GUI appears. Remove it if you want a console for debugging.
 
---
 
## How It Works
 
### Architecture
 
```
WinMain()
│
├── Creates Win32 window (WndProc handles all UI events)
│
├── On [Scan] click:
│   ├── Resolves hostname → IPv4 via getaddrinfo()
│   ├── Fills a thread-safe work queue with port numbers
│   └── Spawns N worker threads (configurable)
│
├── Worker threads (workerThread):
│   ├── Pop port from queue
│   ├── Non-blocking TCP connect with select() timeout
│   ├── On open: PostMessage(WM_SCAN_RESULT) → main thread adds row to ListView
│   └── On progress: PostMessage(WM_SCAN_PROG) → updates progress bar + status
│
└── Monitor thread:
    └── Joins all workers → PostMessage(WM_SCAN_DONE) → re-enables Scan button
```
 
### TCP Probing
 
Each port is tested with a non-blocking `connect()`:
 
1. Create a socket and set it to non-blocking mode (`ioctlsocket` / `fcntl`)
2. Call `connect()` — it returns immediately with `WSAEWOULDBLOCK`
3. Use `select()` with the configured timeout to wait for writability
4. Check `SO_ERROR` via `getsockopt()` — zero means the connection succeeded (port open)
5. Close the socket
This approach means the timeout applies per-port, not per-thread, so hundreds of ports can be probed concurrently without any thread blocking.
 
### Thread Safety
 
- **Work queue** (`std::queue<uint16_t>`) is protected by a `std::mutex`
- **Results** are never written directly from worker threads — they use `PostMessage` to send a heap-allocated `ScanResult*` to the main thread, which owns the ListView control
- **Atomic counters** (`std::atomic<uint32_t>`) track scanned/open counts without locks
### Preset Modes
 
| Preset | Port Range | Threads | Timeout |
|--------|-----------|---------|---------|
| Common | 1 – 1024 | 128 | 1000ms |
| Web | 80 – 8888 | 64 | 800ms |
| Full | 1 – 65535 | 512 | 500ms |
 
---
 
## Usage Guide
 
| Field | Description |
|-------|-------------|
| **Host / IP** | Hostname (e.g. `scanme.nmap.org`) or IPv4 address |
| **Start / End port** | Port range to scan (1–65535) |
| **Threads** | Concurrent connections — higher = faster but more CPU/network load |
| **Timeout (ms)** | How long to wait per port before marking it closed |
 
### Tips
 
- **Fast LAN scan:** 256 threads, 300ms timeout
- **Internet host:** 128 threads, 1000ms timeout (avoid false negatives from latency)
- **Full scan (65535 ports):** Use the "Full" preset; expect 1–5 minutes depending on host and network
- **Firewall note:** Filtered ports (firewalled) are reported as closed because the TCP handshake never completes — this is expected behaviour for a connect-scan
---
 
## Recognised Services
 
The scanner labels the following ports automatically:
 
`FTP (21)` · `SSH (22)` · `Telnet (23)` · `SMTP (25)` · `DNS (53)` · `HTTP (80)` · `POP3 (110)` · `NTP (123)` · `IMAP (143)` · `HTTPS (443)` · `SMB (445)` · `LDAPS (636)` · `IMAPS (993)` · `MySQL (3306)` · `RDP (3389)` · `PostgreSQL (5432)` · `VNC (5900)` · `Redis (6379)` · `HTTP-Alt (8080)` · `MongoDB (27017)` · and more.
 
---
 
## Contributing
 
Contributions are welcome. Some ideas if you want to extend the project:
 
- [ ] UDP scanning
- [ ] Banner grabbing (read first response bytes to identify service version)
- [ ] Export results to CSV / JSON
- [ ] IPv6 support
- [ ] OS detection hints based on TTL / window size
- [ ] Dark mode UI
**To contribute:**
 
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Commit your changes (`git commit -m "Add my feature"`)
4. Push to the branch (`git push origin feature/my-feature`)
5. Open a Pull Request
Please keep PRs focused — one feature or fix per PR makes review easier.
 
---
 
## Legal & Ethical Use
 
> **Only scan hosts you own or have explicit written permission to scan.**
 
Unauthorised port scanning may be:
- Illegal under computer misuse laws in your country (e.g. the Computer Fraud and Abuse Act in the US, the Computer Misuse Act in the UK)
- A violation of your ISP's terms of service
- Treated as hostile reconnaissance by network administrators
**Legitimate uses include:**
- Auditing your own servers and home network
- Penetration testing engagements where you have written authorisation
- Learning and lab environments (e.g. VMs, HTB, TryHackMe)
The authors take no responsibility for misuse of this tool.
 
---
 
## License
 
MIT License — see [LICENSE](LICENSE) for full text.
 
You are free to use, modify, and distribute this software for any purpose, commercial or non-commercial, with attribution.
 
---
 
## Acknowledgements
 
- Inspired by [Nmap](https://nmap.org/) — the gold standard in port scanning
- Built with the Windows API (`winsock2`, `comctl32`) — no third-party libraries
 
