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
private:
    AdbCommunicator();
    ~AdbCommunicator();
    void ReConnect();
    void Close();
    void SendStringToServer(const char* str);
    int ReadBuf(void);

    SOCKET s;
    bool _needsu;
    static AdbCommunicator* _global_adb;

    char actbuf[BUF_SIZE];
    char* actbufpospoint;
    int actbufsize;
    int actbufpos;
};

enum FileTypeEnum { REGFILE, DIRECTORY, LINK, OTHER };

class FileData {
public:
    FileData(std::wstring _name) : type(REGFILE), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0),
        alt_name(_name), name(_name), cache_name(_name) {}
    FileData() : type(REGFILE), mode(0), size(0), accessTime(0),
        modificationTime(0), changeTime(0), uid(0), gid(0) {}
    FileTypeEnum type;
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
std::wstring PathConverter(std::wstring path);
bool ParseStatLine(const std::wstring& line, FileData* fd);
int64_t unixTimeToFileTime(unsigned int utime);
unsigned int fileTimeToUnixTime(int64_t ftime);
std::string FindAdbBinary();

std::list<FileData*>* DirList(std::wstring filename);
void GetStat(WIN32_FIND_DATAW* fs, FileData* fd);
bool RunCommand(std::wstring comm);
