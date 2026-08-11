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
   ---- Adb Communicator -----
   --------------------------- */

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

void AdbCommunicator::SendStringToServer(const char* str) {
    if (send(s, str, strlen(str), 0) == SOCKET_ERROR) {
        Close();
        throw wstring(L"<0009 - could not switch to usb mode>");
    }

    // get result
    char recbuf[5];
    recbuf[4] = '\0';
    ssize_t bytesRead = recv(s, recbuf, 4, MSG_WAITALL);
    if (bytesRead != 4) {
        Close();
        throw wstring(L"<000A - no ack data from adb server>");
    }
    if (strcasecmp("FAIL", recbuf) == 0) {
        // cleanup
        recv(s, recbuf, 4, MSG_WAITALL);
        int datalen = 0;
        sscanf(recbuf, "%x", &datalen);
        if (datalen > 0) {
            char* data = new char[datalen + 1];
            recv(s, data, datalen, MSG_WAITALL);
            delete[] data;
        }
        Close();
        throw wstring(L"<000B - FAIL response from adb server>");
    } else if (strcasecmp("OKAY", recbuf) != 0) {
        Close();
        throw wstring(L"<000C - Bad response from adb server>");
    }
}

void AdbCommunicator::ReConnect() {
    LogA(MSGTYPE_CONNECT, "CONNECT /");
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
    s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw wstring(L"<0006 - socket initialization failed>");
    }
    if (connect(s, result->ai_addr, result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        Close();
        throw wstring(L"<0008 - could not connect to local adb server>");
    }
    freeaddrinfo(result);

    // switch to usb mode
    // TODO: multiple devices support
    SendStringToServer("0012host:transport-usb");
    // start shell
    SendStringToServer("0006shell:");

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
        recv(s, actbuf, BUF_SIZE, 0);
        if (timeout) return;
    }
}

void AdbCommunicator::PushCommandW(wstring command) {
    if (s == INVALID_SOCKET) {
        ReConnect();
        Sleep(500); // wait for the shell to start
    }

    CleanBuffer(false);

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
        Close();
        throw wstring(L"Socket Error");
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
    fs->nFileSizeHigh = (DWORD)(fd->size >> 32);
    fs->nFileSizeLow = (DWORD)(fd->size & 0x0FFFFFFFF);
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
