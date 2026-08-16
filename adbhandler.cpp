#include "adbhandler.h"
#include "adbfsplugin.h"
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

using namespace std;

/* -------------------------------
   ---- Some helper functions ----
   ------------------------------- */

#define EPOCH_DIFF 0x019DB1DED53E8000LL /* 116444736000000000 nsecs */
#define RATE_DIFF 10000000 /* 100 nsecs */

/* Convert a UNIX time_t into a Windows filetime */
int64_t unixTimeToFileTime(unsigned int utime) {
    return ((int64_t)utime * RATE_DIFF) + EPOCH_DIFF;
}

/* Convert a Windows filetime into a UNIX time_t */
unsigned int fileTimeToUnixTime(int64_t ftime) {
    return (unsigned int)((ftime - EPOCH_DIFF) / RATE_DIFF);
}

string trim(string const& str, const char* sepSet) {
    string::size_type const first = str.find_first_not_of(sepSet);
    return (first == string::npos) ? string() : str.substr(first, str.find_last_not_of(sepSet) - first + 1);
}

// The adb shell: channel is a pty, so device tools may emit terminal escape
// sequences (ls colors, prompt titles) that would otherwise end up inside
// "file names". Strips CSI (ESC[...X), OSC (ESC]...BEL/ST) and two-byte ESC
// sequences.
string StripAnsiEscapes(const string& in) {
    string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c != 0x1B) {
            out.push_back((char)c);
            i++;
            continue;
        }
        i++;
        if (i >= in.size()) break;
        if (in[i] == '[') {          // CSI: parameter bytes, then final 0x40-0x7E
            i++;
            while (i < in.size() && ((unsigned char)in[i] < 0x40 || (unsigned char)in[i] > 0x7E)) i++;
            if (i < in.size()) i++;
        } else if (in[i] == ']') {   // OSC: until BEL or ESC-backslash
            i++;
            while (i < in.size() && in[i] != '\a' && in[i] != 0x1B) i++;
            if (i < in.size() && in[i] == '\a') i++;
            else if (i + 1 < in.size() && in[i] == 0x1B && in[i + 1] == '\\') i += 2;
        } else {
            i++;                     // ESC + single character
        }
    }
    return out;
}

wstring PathConverter(wstring path) {
    for (auto& c : path)
        if (c == L'\\') c = L'/';
    return path;
}

// The <0XXX - ...> pseudo-entries a failed listing produces are status
// messages, not files — every file operation must refuse them instead of
// sending the marker text to the device as a path.
bool IsErrorMarker(const wstring& path) {
    size_t sl = path.find_last_of(L"/\\");
    wstring base = (sl == wstring::npos) ? path : path.substr(sl + 1);
    return base.size() >= 3 && base[0] == L'<' && base[1] == L'0' && base.back() == L'>';
}

// adb binary discovery: $ADBFS_ADB, then PATH, then common macOS install locations
string FindAdbBinary() {
    const char* env = getenv("ADBFS_ADB");
    if (env && *env) return env;
    vector<string> dirs;
    if (const char* path = getenv("PATH")) {
        string p = path;
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == string::npos) end = p.size();
            if (end > start) dirs.push_back(p.substr(start, end - start));
            start = end + 1;
        }
    }
    dirs.push_back("/opt/homebrew/bin");
    dirs.push_back("/usr/local/bin");
    if (const char* home = getenv("HOME"))
        dirs.push_back(string(home) + "/Library/Android/sdk/platform-tools");
    for (auto& d : dirs) {
        string candidate = d + "/adb";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "adb";
}

// quote a string for usage in bash
wstring QuoteString(wstring str) {
    wstring result = L"'";
    for (auto i = str.begin(); i != str.end(); i++) {
        if (*i == L'\'') result.append(L"'\\''");
        else result.push_back(*i);
    }
    result.append(L"'");
    return result;
}

unsigned char base64table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
unsigned char base64table2[257] = "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\x3E~~~\x3F\x34\x35\x36\x37\x38\x39\x3A\x3B\x3C\x3D~~~\x00~~~\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19~~~~~~\x1A\x1B\x1C\x1D\x1E\x1F\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2A\x2B\x2C\x2D\x2E\x2F\x30\x31\x32\x33~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~";

// base64 decode 4 characters to 3 characters
// returns the bytes decoded (might be less because of padding)
int decode64(const char* input, char* output) {
    if ((input[3] == '=') && (input[2] == '=') && (input[0] == '=') && (input[1] == '=')) return 0;
    unsigned int n = (base64table2[(unsigned char)input[0]] << 18) | (base64table2[(unsigned char)input[1]] << 12) |
                     (base64table2[(unsigned char)input[2]] << 6) | (base64table2[(unsigned char)input[3]]);
    output[0] = (char)(n >> 16);
    output[1] = (char)((n >> 8) & 0xFF);
    output[2] = (char)(n & 0xFF);
    return 1 + ((input[3] != '=') ? 1 : 0) + ((input[2] != '=') ? 1 : 0);
}

int encode64(const char* input, char* output) {
    unsigned int n = ((unsigned char)input[0] << 16) | ((unsigned char)input[1] << 8) | (unsigned char)input[2];
    output[0] = base64table[n >> 18];
    output[1] = base64table[(n >> 12) & 0x3F];
    output[2] = base64table[(n >> 6) & 0x3F];
    output[3] = base64table[n & 0x3F];
    return 4;
}

/* ---------------------------
   ---- Transfer-mode setting -
   --------------------------- */

// wfx.ini is shared by every WFX plugin, so reads and writes must keep all
// foreign sections and keys intact.
static string configIniPath;
static const char* kIniSection = "[adbfsplugin]";
static const char* kIniKey = "TransferMode=";

void SetConfigIniPath(const string& path) {
    configIniPath = path;
}

static vector<string> readIniLines() {
    vector<string> lines;
    FILE* f = fopen(configIniPath.c_str(), "rb");
    if (!f) return lines;
    string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back((char)c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    fclose(f);
    return lines;
}

TransferModeEnum GetTransferMode() {
    const char* env = getenv("ADBFS_TRANSFER_MODE");
    if (env && *env) return (string(env) == "shell") ? TRANSFER_SHELL : TRANSFER_SYNC;
    bool insection = false;
    for (auto& line : readIniLines()) {
        string t = trim(line, " \t");
        if (!t.empty() && t.front() == '[') {
            insection = (t == kIniSection);
        } else if (insection && t.compare(0, strlen(kIniKey), kIniKey) == 0) {
            return (trim(t.substr(strlen(kIniKey)), " \t") == "shell") ? TRANSFER_SHELL : TRANSFER_SYNC;
        }
    }
    return TRANSFER_SYNC;
}

void SaveTransferMode(TransferModeEnum mode) {
    string entry = string(kIniKey) + (mode == TRANSFER_SHELL ? "shell" : "sync");
    vector<string> lines = readIniLines();
    bool insection = false, written = false;
    size_t sectionend = lines.size();   // insertion point if the key is absent
    bool sectionfound = false;
    for (size_t i = 0; i < lines.size(); i++) {
        string t = trim(lines[i], " \t");
        if (!t.empty() && t.front() == '[') {
            insection = (t == kIniSection);
            if (insection) sectionfound = true;
        } else if (insection && !written && t.compare(0, strlen(kIniKey), kIniKey) == 0) {
            lines[i] = entry;
            written = true;
        }
        if (insection) sectionend = i + 1;
    }
    if (!written) {
        if (!sectionfound) {
            lines.push_back(kIniSection);
            lines.push_back(entry);
        } else {
            lines.insert(lines.begin() + sectionend, entry);
        }
    }
    // Rewrite atomically: wfx.ini holds every WFX plugin's settings, so a
    // failed write must never leave it truncated. Write a sibling temp file
    // and rename() it over the target — resolving a symlinked ini to its
    // real location first, so the link itself survives.
    string target = configIniPath;
    if (char* resolved = realpath(configIniPath.c_str(), NULL)) {
        target = resolved;
        free(resolved);
    }
    string tmppath = target + ".adbfsplugin.tmp";
    FILE* f = fopen(tmppath.c_str(), "wb");
    if (!f) {
        LogA(MSGTYPE_IMPORTANTERROR, "Could not save transfer mode (wfx.ini directory not writable)");
        return;
    }
    bool ok = true;
    for (auto& line : lines) {
        if (fputs(line.c_str(), f) == EOF || fputc('\n', f) == EOF) {
            ok = false;
            break;
        }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmppath.c_str(), target.c_str()) != 0) {
        unlink(tmppath.c_str());
        LogA(MSGTYPE_IMPORTANTERROR, "Could not save transfer mode (write to wfx.ini failed)");
    }
}

/* ---------------------------
   ---- Adb Communicator -----
   --------------------------- */

// Shared connection primitives, defined in the sync-protocol section below.
static SOCKET AdbServerConnect();
static void AdbRequest(SOCKET s, const string& payload);
static void AdbSelectTransport(SOCKET s);

AdbCommunicator* AdbCommunicator::_global_adb = 0;

AdbCommunicator::~AdbCommunicator() {
    Close();
    LogA(MSGTYPE_DISCONNECT, "Closing plugin");
}

AdbCommunicator::AdbCommunicator() {
    string adb = FindAdbBinary();
    LogA(MSGTYPE_DETAILS, ("Starting ADB server: " + adb).c_str());
    pid_t pid = 0;
    const char* argv[] = { adb.c_str(), "start-server", NULL };
    // upstream threw when adb couldn't be spawned; here we log and still try to
    // connect — the server may already be running
    if (posix_spawnp(&pid, adb.c_str(), NULL, NULL, (char* const*)argv, environ) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);   // adb start-server returns once the daemon is up
    } else {
        LogA(MSGTYPE_IMPORTANTERROR, "Could not run 'adb start-server' (is adb installed?), trying to connect anyway");
    }
    s = INVALID_SOCKET;
    _needsu = (getenv("ADBFS_NO_SU") == NULL);
    _toolmode = -1;
    actbufsize = 0;
    actbufpos = 0;
    actbufpospoint = actbuf;
}

// runs `<toolbox> echo adbfsprobe` on the device and checks for the echo
static bool ProbeEcho(const wstring& command) {
    AdbCommunicator::instance()->PushCommandW(command);
    bool found = false;
    string* line = AdbCommunicator::instance()->ReadLine();
    while (line != NULL) {
        if (*line == "adbfsprobe") found = true;
        delete line;
        line = AdbCommunicator::instance()->ReadLine();
    }
    return found;
}

int AdbCommunicator::ToolMode() {
    if (_toolmode >= 0) return _toolmode;
    try {
        _toolmode = 2;   // set before probing: the probes recurse into PushCommandW
        if (ProbeEcho(L"busybox echo adbfsprobe")) _toolmode = 0;
        else if (ProbeEcho(L"toybox echo adbfsprobe")) _toolmode = 1;
    } catch (wstring&) {
        _toolmode = -1;  // connection failed — probe again next time
        throw;
    }
    return _toolmode;
}

wstring Tool(const wchar_t* applet) {
    switch (AdbCommunicator::instance()->ToolMode()) {
        case 0: return wstring(L"busybox ") + applet;
        case 1: return wstring(L"toybox ") + applet;
        default: return applet;
    }
}

void AdbCommunicator::Close() {
    LogA(MSGTYPE_DISCONNECT, "Closing connection /");
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
    actbufsize = 0;
    actbufpos = 0;
}

void AdbCommunicator::ReConnect() {
    LogA(MSGTYPE_CONNECT, "CONNECT /");
    s = AdbServerConnect();
    try {
        AdbSelectTransport(s);
        AdbRequest(s, "shell:");
    } catch (wstring&) {
        Close();
        throw;
    }

    if (_needsu) {
        Sleep(500);         // let the shell start
        CleanBuffer(false); // remove everything in buffer
        send(s, "su\n", 3, 0);
        Sleep(50);          // small timeout for the echo
        CleanBuffer(false); // remove echo
        CleanBuffer(true);  // wait for root
    }
}

void AdbCommunicator::CleanBuffer(bool timeout) {
    // cleans input buffer
    actbufsize = 0;
    actbufpos = 0;
    actbufpospoint = actbuf;
    for (;;) {
        TIMEVAL tv;
        // timeout=true waits for a response (e.g. su result) — but bounded:
        // a fast device may have answered inside the previous drain window
        // already, and an unbounded select() then deadlocks the panel
        tv.tv_sec = timeout ? 2 : 0;
        tv.tv_usec = 0;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        if (select(s + 1, &set, NULL, NULL, &tv) <= 0) return;
        // EOF/error also counts as "readable": without this check a dead
        // connection (device vanished) spins this loop forever on the UI
        // thread — select flags the socket, recv keeps returning <= 0
        if (recv(s, actbuf, BUF_SIZE, 0) <= 0) {
            Close();
            return;
        }
        if (timeout) return;
    }
}

void AdbCommunicator::PushCommandW(wstring command) {
    if (s == INVALID_SOCKET) {
        ReConnect();
        Sleep(500); // wait for the shell to start
    }

    CleanBuffer(false);
    if (s == INVALID_SOCKET) {  // the drain found the connection dead
        ReConnect();
        Sleep(500);
    }

    // add some garbage data to determine where sending starts and where it stops
    command = L"echo \"===adbfsplugin<--\" ;" + command + L" ; echo \"===adbfsplugin-->\"";

    string comm = ws_to_utf8(command);
    comm.push_back('\n');
    if (send(s, comm.c_str(), comm.size(), 0) == SOCKET_ERROR) {
        Close();
        throw wstring(L"<000D - Command send failed>");
    }

    // throw out initial garbage (the shell echoing the command back)
    string* line = ReadLine();
    while ((line != NULL) && (*line != "===adbfsplugin<--")) {
        delete line;
        line = ReadLine();
    }
    if (line) delete line;
}

int AdbCommunicator::ReadBuf(void) {
    actbufpos++;
    actbufpospoint++;
    if (actbufsize <= actbufpos) {
        ssize_t n = recv(s, actbuf, BUF_SIZE, 0);
        actbufpos = 0;
        actbufpospoint = actbuf;
        if (n < 0) {
            actbufsize = 0;
            return SOCKET_ERROR;
        }
        actbufsize = (int)n;
    }
    return actbufsize - actbufpos;
}

int AdbCommunicator::PutData(const char* data, int length) {
    return (int)send(s, data, length, 0);
}

string* AdbCommunicator::ReadLine() {
    string input = "";
    int bytesRead = ReadBuf();
    int size = 0;
    while ((bytesRead != SOCKET_ERROR) && (bytesRead != 0) && (*actbufpospoint != '\n') &&
           ((size != 17) || (input != "===adbfsplugin-->"))) {
        size++;
        input.push_back(*actbufpospoint);
        bytesRead = ReadBuf();
    }
    if (bytesRead == SOCKET_ERROR) {
        bool timedout = (errno == EAGAIN || errno == EWOULDBLOCK);   // SO_RCVTIMEO expired
        Close();
        throw timedout ? wstring(L"<000E - device not answering (read timeout)>")
                       : wstring(L"<000F - socket error>");
    }
    if (input.empty() || input == "===adbfsplugin-->") {
        return NULL;
    }
    string cleaned = trim(StripAnsiEscapes(input), " \t\r\n");
    if (cleaned == "===adbfsplugin-->") {
        return NULL;   // end marker wrapped in escapes by the terminal
    }
    return new string(cleaned);
}

wstring* AdbCommunicator::ReadLineW() {
    string* input = ReadLine();
    if (input == NULL) return NULL;
    wstring* result = new wstring(utf8_to_ws(*input));
    delete input;
    return result;
}

/* ---------------------------
   ---- ADB sync protocol ----
   --------------------------- */

// The sync service transfers file bodies the way adb pull/push does: binary
// frames of 4-byte id + little-endian uint32 length, DATA payloads capped at
// 64 KB. Each transfer uses its own connection so the interactive shell
// stays undisturbed.
#define SYNC_DATA_MAX 65536

static bool SendAllRaw(SOCKET s, const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n > 0) {
        ssize_t w = send(s, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool RecvAllRaw(SOCKET s, void* data, size_t n) {
    char* p = (char*)data;
    while (n > 0) {
        ssize_t r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// Connect to the local ADB server.
// The inactivity timeout on reads: a silently-dead device link (Wi-Fi gone
// without the adb server noticing) must error out instead of blocking DC's
// UI thread in recv forever. ADBFS_READ_TIMEOUT seconds overrides the
// default; 0 disables — needed if silent long-running commands (a huge
// rm -r / cp) legitimately produce no output for longer.
// SO_NOSIGPIPE: a peer dying mid-send must surface as an error, not kill
// the commander with SIGPIPE.
static SOCKET AdbServerConnect() {
    struct addrinfo* result = NULL;
    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const char* port = getenv("ANDROID_ADB_SERVER_PORT");
    if (!port || !*port) port = "5037";
    if (getaddrinfo("127.0.0.1", port, &hints, &result) != 0) {
        throw wstring(L"<0007 - localhost not found>");
    }
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw wstring(L"<0006 - socket initialization failed>");
    }
    if (connect(s, result->ai_addr, result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        closesocket(s);
        throw wstring(L"<0008 - could not connect to local adb server>");
    }
    freeaddrinfo(result);
    int rcvsecs = 30;
    const char* toenv = getenv("ADBFS_READ_TIMEOUT");
    if (toenv && *toenv) rcvsecs = atoi(toenv);
    if (rcvsecs > 0) {
        TIMEVAL rcvto;
        rcvto.tv_sec = rcvsecs;
        rcvto.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
        // and on sends: a wedged link mid-upload fills the socket buffer and
        // send() would otherwise block the commander's UI thread forever
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &rcvto, sizeof(rcvto));
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return s;
}

// One smart-socket request: <4-hex-len><payload>, expect OKAY.
static void AdbRequest(SOCKET s, const string& payload) {
    char lenbuf[8];
    snprintf(lenbuf, sizeof(lenbuf), "%04x", (unsigned)payload.size());
    string req = string(lenbuf) + payload;
    if (!SendAllRaw(s, req.data(), req.size())) {
        throw wstring(L"<000D - Command send failed>");
    }
    char recbuf[5] = {0};
    if (recv(s, recbuf, 4, MSG_WAITALL) != 4) {
        throw wstring(L"<000A - no ack data from adb server>");
    }
    if (strcasecmp("FAIL", recbuf) == 0) {
        throw wstring(L"<000B - FAIL response from adb server>");
    }
    if (strcasecmp("OKAY", recbuf) != 0) {
        throw wstring(L"<000C - Bad response from adb server>");
    }
}

// Select the device: ADBFS_SERIAL pins a specific one (needed with several
// devices attached); otherwise transport-any takes the single connected
// device whatever its transport — USB or wireless TCP (transport-usb would
// FAIL for wireless devices).
static void AdbSelectTransport(SOCKET s) {
    const char* serial = getenv("ADBFS_SERIAL");
    if (serial && *serial) {
        AdbRequest(s, string("host:transport:") + serial);
    } else {
        AdbRequest(s, "host:transport-any");
    }
}

static bool SyncSendHeader(SOCKET s, const char* id, uint32_t len) {
    char hdr[8];
    memcpy(hdr, id, 4);
    memcpy(hdr + 4, &len, 4);   // little-endian on every supported arch
    return SendAllRaw(s, hdr, 8);
}

static bool SyncSendFrame(SOCKET s, const char* id, const string& payload) {
    return SyncSendHeader(s, id, (uint32_t)payload.size()) &&
           SendAllRaw(s, payload.data(), payload.size());
}

// Drain a FAIL payload (the device's error text) into the log.
static void SyncLogFail(SOCKET s, uint32_t len) {
    if (len > 4096) len = 4096;
    string msg(len, 0);
    if (len == 0 || !RecvAllRaw(s, &msg[0], len)) msg = "(no error message)";
    wstring wmsg = L"sync: " + utf8_to_ws(msg);
    if (msg.find("ermission") != string::npos)
        wmsg += L" - the device-shell transfer mode (plugin Configure button) can reach root-only files";
    LogT(MSGTYPE_IMPORTANTERROR, wmsg);
}

// Open a sync-service connection ready for RECV/SEND requests.
static SOCKET SyncConnect() {
    SOCKET s = AdbServerConnect();
    try {
        AdbSelectTransport(s);
        AdbRequest(s, "sync:");
    } catch (wstring&) {
        closesocket(s);
        throw;
    }
    return s;
}

int SyncPull(const wstring& remote, const wstring& local, int64_t expectedSize) {
    string local8 = ws_to_utf8(local);
    SOCKET s;
    try {
        s = SyncConnect();
    } catch (wstring& e) {
        LogT(MSGTYPE_IMPORTANTERROR, e);
        return FS_FILE_READERROR;
    }
    FILE* f = fopen(local8.c_str(), "wb");
    if (f == NULL) {
        closesocket(s);
        return FS_FILE_WRITEERROR;
    }
    int result = FS_FILE_READERROR;
    if (expectedSize <= 0) expectedSize = 1;
    int64_t saved = 0;
    ProgressT(remote, local, 0);
    if (SyncSendFrame(s, "RECV", ws_to_utf8(PathConverter(remote)))) {
        char buf[SYNC_DATA_MAX];
        for (;;) {
            char id[4];
            uint32_t len = 0;
            if (!RecvAllRaw(s, id, 4) || !RecvAllRaw(s, &len, 4)) {
                LogT(MSGTYPE_IMPORTANTERROR, L"sync: connection lost during download");
                break;
            }
            if (memcmp(id, "DONE", 4) == 0) {
                SyncSendHeader(s, "QUIT", 0);
                ProgressT(remote, local, 100);
                result = FS_FILE_OK;
                break;
            }
            if (memcmp(id, "FAIL", 4) == 0) {
                SyncLogFail(s, len);
                break;
            }
            if (memcmp(id, "DATA", 4) != 0 || len > SYNC_DATA_MAX) {
                LogT(MSGTYPE_IMPORTANTERROR, L"sync: unexpected response from device");
                break;
            }
            if (!RecvAllRaw(s, buf, len)) {
                LogT(MSGTYPE_IMPORTANTERROR, L"sync: connection lost during download");
                break;
            }
            if (fwrite(buf, 1, len, f) != len) {
                result = FS_FILE_WRITEERROR;
                break;
            }
            saved += len;
            // a stale listing size must not push >100 to the commander
            int pct = (int)((double)saved / expectedSize * 100);
            if (ProgressT(remote, local, pct > 100 ? 100 : pct)) {
                result = FS_FILE_USERABORT;
                break;
            }
        }
    }
    closesocket(s);
    fclose(f);
    if (result != FS_FILE_OK) unlink(local8.c_str());   // no partial downloads
    return result;
}

int SyncPush(const wstring& local, const wstring& remote) {
    string local8 = ws_to_utf8(local);
    struct stat st;
    if (stat(local8.c_str(), &st) != 0) return FS_FILE_READERROR;
    FILE* f = fopen(local8.c_str(), "rb");
    if (f == NULL) return FS_FILE_READERROR;
    SOCKET s;
    try {
        s = SyncConnect();
    } catch (wstring& e) {
        LogT(MSGTYPE_IMPORTANTERROR, e);
        fclose(f);
        return FS_FILE_WRITEERROR;
    }
    int result = FS_FILE_WRITEERROR;
    int64_t fullsize = (st.st_size > 0) ? (int64_t)st.st_size : 1;
    int64_t sent = 0;
    ProgressT(local, remote, 0);
    string arg = ws_to_utf8(PathConverter(remote)) + "," + std::to_string((unsigned)st.st_mode);
    if (SyncSendFrame(s, "SEND", arg)) {
        char buf[SYNC_DATA_MAX];
        bool aborted = false, senderr = false;
        size_t n;
        while ((n = fread(buf, 1, SYNC_DATA_MAX, f)) > 0) {
            if (!SyncSendHeader(s, "DATA", (uint32_t)n) || !SendAllRaw(s, buf, n)) {
                senderr = true;   // adbd may FAIL early and close its read side
                break;
            }
            sent += n;
            // a file growing mid-push must not push >100 to the commander
            int pct = (int)((double)sent / fullsize * 100);
            if (ProgressT(local, remote, pct > 100 ? 100 : pct)) {
                aborted = true;
                break;
            }
        }
        if (aborted) {
            result = FS_FILE_USERABORT;
        } else {
            if (!senderr) SyncSendHeader(s, "DONE", (uint32_t)st.st_mtime);
            char id[4];
            uint32_t len = 0;
            if (!RecvAllRaw(s, id, 4) || !RecvAllRaw(s, &len, 4)) {
                LogT(MSGTYPE_IMPORTANTERROR, L"sync: no final status from device");
            } else if (memcmp(id, "OKAY", 4) == 0) {
                SyncSendHeader(s, "QUIT", 0);
                ProgressT(local, remote, 100);
                result = FS_FILE_OK;
            } else if (memcmp(id, "FAIL", 4) == 0) {
                SyncLogFail(s, len);
            } else {
                LogT(MSGTYPE_IMPORTANTERROR, L"sync: unexpected response from device");
            }
        }
    }
    closesocket(s);
    fclose(f);
    return result;
}

/* ---------------------------
   ---- FileData Helpers -----
   --------------------------- */

// Parses one line of `busybox stat -c "%a -%F- %g %u %s %X %Y %Z %N"` output.
// Example: 644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 'file one'
bool ParseStatLine(const wstring& line, FileData* fd) {
    size_t pos = 0;
    auto skipSpaces = [&] { while (pos < line.size() && line[pos] == L' ') pos++; };
    auto parseUInt = [&](uint64_t* out) -> bool {
        skipSpaces();
        size_t start = pos;
        uint64_t v = 0;
        while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'9') {
            v = v * 10 + (uint64_t)(line[pos] - L'0');
            pos++;
        }
        *out = v;
        return pos > start;
    };

    skipSpaces();
    size_t start = pos;
    unsigned int mode = 0;
    while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'7') {
        mode = mode * 8 + (unsigned int)(line[pos] - L'0');
        pos++;
    }
    if (pos == start) return false;

    skipSpaces();
    if (pos >= line.size() || line[pos] != L'-') return false;
    pos++;
    size_t typeEnd = line.find(L'-', pos);
    if (typeEnd == wstring::npos) return false;
    wstring type = line.substr(pos, typeEnd - pos);
    pos = typeEnd + 1;

    uint64_t gid, uid, size, atime, mtime, ctime;
    if (!parseUInt(&gid) || !parseUInt(&uid) || !parseUInt(&size) ||
        !parseUInt(&atime) || !parseUInt(&mtime) || !parseUInt(&ctime))
        return false;

    if (pos < line.size() && line[pos] == L' ') pos++;
    if (pos >= line.size()) return false;   // name is mandatory

    fd->mode = mode;
    fd->gid = (unsigned int)gid;
    fd->uid = (unsigned int)uid;
    fd->size = (int64_t)size;
    fd->accessTime = (unsigned int)atime;
    fd->modificationTime = (unsigned int)mtime;
    fd->changeTime = (unsigned int)ctime;
    fd->type = OTHER;
    if (type == L"directory") fd->type = DIRECTORY;
    else if (type == L"symbolic link") fd->type = LINK;
    else if (type == L"regular file" || type == L"regular empty file") fd->type = REGFILE;
    fd->alt_name = line.substr(pos);
    return true;
}

void FillStat(wstring directory, list<FileData*>* fd) {
    try {
        // busybox stat supports %N (quoted name + link target); toybox/plain
        // stat may not, so fall back to %n there
        wstring command = Tool(L"stat") +
            (AdbCommunicator::instance()->ToolMode() == 0
                 ? L" -c \"%a -%F- %g %u %s %X %Y %Z %N\" "
                 : L" -c \"%a -%F- %g %u %s %X %Y %Z %n\" ");
        for (auto i = fd->begin(); i != fd->end(); i++) {
            command.append(L" ");
            command.append(QuoteString(directory + (*i)->name));
        }
        AdbCommunicator::instance()->PushCommandW(command);
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        auto i = fd->begin();
        while ((line != NULL) && (i != fd->end())) {
            (*i)->cache_name = directory + (*i)->name;
            if (!ParseStatLine(*line, *i)) {
                (*i)->alt_name = L"<0005 - stat failed>";
            }
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
            i++;
        }
        if (line) delete line;

        // Second pass: symlinks (e.g. /sdcard) get the type/size/times of
        // their target via stat -L so directory links are enterable; a
        // dangling link keeps type LINK.
        list<FileData*> links;
        for (auto j = fd->begin(); j != fd->end(); j++) {
            if ((*j)->type == LINK) {
                (*j)->islink = true;
                links.push_back(*j);
            }
        }
        if (!links.empty()) {
            command = Tool(L"stat") + L" -L -c \"%a -%F- %g %u %s %X %Y %Z %n\" ";
            for (auto j = links.begin(); j != links.end(); j++) {
                command.append(L" ");
                command.append(QuoteString(directory + (*j)->name));
            }
            AdbCommunicator::instance()->PushCommandW(command);
            line = AdbCommunicator::instance()->ReadLineW();
            auto k = links.begin();
            while ((line != NULL) && (k != links.end())) {
                FileData target;
                if (ParseStatLine(*line, &target)) {
                    (*k)->type = target.type;
                    (*k)->mode = target.mode;
                    (*k)->size = target.size;
                    (*k)->accessTime = target.accessTime;
                    (*k)->modificationTime = target.modificationTime;
                    (*k)->changeTime = target.changeTime;
                    (*k)->uid = target.uid;
                    (*k)->gid = target.gid;
                }
                delete line;
                line = AdbCommunicator::instance()->ReadLineW();
                k++;
            }
            if (line) delete line;
        }
    } catch (wstring&) {
    }
}

void GetStat(WIN32_FIND_DATAW* fs, FileData* fd) {
    memset(fs, 0, sizeof(WIN32_FIND_DATAW));
    ws_to_u16buf(fs->cFileName, countof(fs->cFileName), fd->name);
    fs->dwFileAttributes = FILE_ATTRIBUTE_UNIX_MODE;
    if (fd->islink) {
        fs->dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    }
    if (fd->type == DIRECTORY) {
        fs->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    fs->dwReserved0 = fd->mode;
    int64_t ft = unixTimeToFileTime(fd->modificationTime);
    fs->ftLastWriteTime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
    fs->ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
    // report 0 for directories: DC shows <DIR>/<LNK> in the size column only
    // then, and the inode size from stat is meaningless to the user anyway
    int64_t size = (fd->type == DIRECTORY) ? 0 : fd->size;
    fs->nFileSizeHigh = (DWORD)(size >> 32);
    fs->nFileSizeLow = (DWORD)(size & 0x0FFFFFFFF);
}

list<FileData*>* DirList(wstring filename) {
    auto* result = new list<FileData*>();
    try {
        // `| cat` makes ls's stdout a pipe, not the pty — suppressing every
        // tty-only behavior at the source (colors, backslash-escaped spaces)
        AdbCommunicator::instance()->PushCommandW(Tool(L"ls") + L" -1 " + QuoteString(filename) + L" | cat");
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        while (line != NULL) {
            result->push_back(new FileData(*line));
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
        }
    } catch (wstring& e) {
        result->push_back(new FileData(e));
    }

    // Get stat in batches of 10 files
    auto* l = new list<FileData*>();
    for (auto i = result->begin(); i != result->end(); i++) {
        l->push_back(*i);
        if (l->size() > 10) {
            FillStat(filename, l);
            l->clear();
        }
    }
    if (!l->empty()) {
        FillStat(filename, l);
    }
    delete l;
    return result;
}

bool RunCommand(wstring comm) {
    try {
        AdbCommunicator::instance()->PushCommandW(comm);
        wstring* line = AdbCommunicator::instance()->ReadLineW();
        while (line != NULL) {
            LogT(MSGTYPE_DETAILS, *line);
            delete line;
            line = AdbCommunicator::instance()->ReadLineW();
        }
        return true;
    } catch (wstring& e) {
        LogT(MSGTYPE_IMPORTANTERROR, e);
        return false;
    }
}
