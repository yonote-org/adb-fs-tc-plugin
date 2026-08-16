#pragma once
#include "platform.h"
#include "wfxcompat.h"

#define BUF_SIZE 8192

// Singleton talking to the local ADB server (smart-socket protocol + device shell)
class AdbCommunicator {
public:
    static AdbCommunicator* instance() { if (!_global_adb) _global_adb = new AdbCommunicator(); return _global_adb; }
    static void disconnect() { if (_global_adb) delete _global_adb; _global_adb = NULL; }
    std::wstring* ReadLineW();
    std::string* ReadLine();
    int PutData(const char* data, int length);
    void CleanBuffer(bool timeout);
    void PushCommandW(std::wstring command);
    void SetSU(bool needsu) { _needsu = needsu; }
    // Which toolbox the device offers: 0 = busybox, 1 = toybox (stock
    // Android), 2 = plain shell applets. Probed lazily, once per connection.
    int ToolMode();
private:
    AdbCommunicator();
    ~AdbCommunicator();
    void ReConnect();
    void Close();
    int ReadBuf(void);

    SOCKET s;
    bool _needsu;
    int _toolmode;
    static AdbCommunicator* _global_adb;

    char actbuf[BUF_SIZE];
    char* actbufpospoint;
    int actbufsize;
    int actbufpos;
};

enum FileTypeEnum { REGFILE, DIRECTORY, LINK, OTHER };

class FileData {
public:
    FileData(std::wstring _name) : type(REGFILE), islink(false), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0),
        alt_name(_name), name(_name), cache_name(_name) {}
    FileData() : type(REGFILE), islink(false), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0) {}
    FileTypeEnum type;      // for symlinks: the followed target's type
    bool islink;
    unsigned int mode;
    int64_t size;
    unsigned int accessTime, modificationTime, changeTime;
    unsigned int uid, gid;
    std::wstring alt_name;
    std::wstring name;
    std::wstring cache_name;
};

int decode64(const char* input, char* output);
int encode64(const char* input, char* output);
std::wstring QuoteString(std::wstring str);
std::string trim(std::string const& str, const char* sepSet);
std::string StripAnsiEscapes(const std::string& in);
std::wstring PathConverter(std::wstring path);
bool ParseStatLine(const std::wstring& line, FileData* fd);
int64_t unixTimeToFileTime(unsigned int utime);
unsigned int fileTimeToUnixTime(int64_t ftime);
std::string FindAdbBinary();
std::wstring Tool(const wchar_t* applet);   // applet prefixed per ToolMode()
bool IsErrorMarker(const std::wstring& path);  // <0XXX - ...> pseudo-entry?

std::list<FileData*>* DirList(std::wstring filename);
void GetStat(WIN32_FIND_DATAW* fs, FileData* fd);
bool RunCommand(std::wstring comm);

// Transfer-mode setting, persisted in the commander's wfx.ini (the path
// FsSetDefaultParams provides). ADBFS_TRANSFER_MODE=sync|shell overrides it.
enum TransferModeEnum { TRANSFER_SYNC, TRANSFER_SHELL };
void SetConfigIniPath(const std::string& path);
TransferModeEnum GetTransferMode();
void SaveTransferMode(TransferModeEnum mode);

// Native ADB sync-protocol transfers (what adb pull/push speaks), each on its
// own connection to the adb server. Drive ProgressT and return FS_FILE_*.
int SyncPull(const std::wstring& remote, const std::wstring& local, int64_t expectedSize);
int SyncPush(const std::wstring& local, const std::wstring& remote);
