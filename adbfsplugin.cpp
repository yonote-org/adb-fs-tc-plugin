// adbfsplugin.cpp : WFX plugin entry points (Double Commander / Total Commander API)

#include "platform.h"
#include "adbfsplugin.h"
#include "adbhandler.h"
#include "wfxcompat.h"

#define pluginrootlen 1

using namespace std;

int PluginNumber;
tProgressProc ProgressProc = NULL;
tLogProc LogProc = NULL;
tRequestProc RequestProc = NULL;
tProgressProcW ProgressProcW = NULL;
tLogProcW LogProcW = NULL;
tRequestProcW RequestProcW = NULL;
map<wstring, FileData> cacheMap;

DCEXPORT int DCPCALL FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc pLogProc, tRequestProc pRequestProc) {
    ProgressProc = pProgressProc;
    LogProc = pLogProc;
    RequestProc = pRequestProc;
    PluginNumber = PluginNr;
    return 0;
}

DCEXPORT int DCPCALL FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW pLogProcW, tRequestProcW pRequestProcW) {
    ProgressProcW = pProgressProcW;
    LogProcW = pLogProcW;
    RequestProcW = pRequestProcW;
    PluginNumber = PluginNr;
    return 0;
}

typedef struct {
    list<FileData*>* result;
    wstring path;
    int origlength;
} FindDataHandle;

DCEXPORT HANDLE DCPCALL FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    cacheMap.clear();
    wstring path = PathConverter(u16_to_ws(Path));
    if (path.empty() || path.back() != L'/') {
        path.push_back(L'/');
    }
    list<FileData*>* result = DirList(path);
    memset(FindData, 0, sizeof(WIN32_FIND_DATAW));
    if (result->empty()) {
        delete result;
        return INVALID_HANDLE_VALUE;
    }
    for (auto i = result->begin(); i != result->end(); i++) {
        cacheMap[(*i)->cache_name] = **i;
    }
    FindDataHandle* r = new FindDataHandle;
    r->path = path;

    FileData* back = result->back();
    result->pop_back();
    GetStat(FindData, back);
    delete back;

    r->result = result;
    r->origlength = (int)result->size();
    return r;
}

DCEXPORT HANDLE DCPCALL FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    WIN32_FIND_DATAW FindDataW;
    WCHAR PathW[wdirtypemax];
    HANDLE retval = FsFindFirstW(awfilenamecopy(PathW, Path), &FindDataW);
    if (retval != INVALID_HANDLE_VALUE)
        copyfinddatawa(FindData, &FindDataW);
    return retval;
}

DCEXPORT BOOL DCPCALL FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    FindDataHandle* r = (FindDataHandle*)Hdl;
    list<FileData*>* result = r->result;
    if (result->empty()) {
        return 0;
    }
    FileData* str = result->back();
    result->pop_back();
    GetStat(FindData, str);
    delete str;
    return 1;
}

DCEXPORT BOOL DCPCALL FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    WIN32_FIND_DATAW FindDataW;
    BOOL retval = FsFindNextW(Hdl, &FindDataW);
    if (retval)
        copyfinddatawa(FindData, &FindDataW);
    return retval;
}

DCEXPORT int DCPCALL FsFindClose(HANDLE Hdl) {
    if (Hdl == NULL || Hdl == INVALID_HANDLE_VALUE || Hdl == (HANDLE)(intptr_t)1)
        return 0;
    FindDataHandle* r = (FindDataHandle*)Hdl;
    list<FileData*>* result = r->result;
    while (!result->empty()) {
        delete result->back();
        result->pop_back();
    }
    delete result;
    delete r;
    return 0;
}

DCEXPORT BOOL DCPCALL FsMkDirW(WCHAR* Path) {
    return RunCommand(Tool(L"mkdir") + L" " + QuoteString(PathConverter(u16_to_ws(Path))));
}

DCEXPORT BOOL DCPCALL FsMkDir(char* Path) {
    WCHAR wbuf[wdirtypemax];
    return FsMkDirW(awfilenamecopy(wbuf, Path));
}

// The Configure button in the commander's plugin settings: a Yes/No choice
// between the two transfer modes, saved to the shared wfx.ini.
static int ShowConfigDialog() {
    wstring title = L"ADB Plugin Configuration";
    wstring text = wstring(L"Transfer files with the fast ADB sync protocol (what adb pull/push uses)?\n\n") +
        L"Yes = sync protocol: fast native transfers (default)\n" +
        L"No = device shell: base64 transfers, slower, but with su they reach root-only files\n\n" +
        L"Current mode: " + (GetTransferMode() == TRANSFER_SYNC ? L"sync protocol" : L"device shell");
    const char* override_ = getenv("ADBFS_TRANSFER_MODE");
    if (override_ && *override_) {
        text += L"\n\nNote: the ADBFS_TRANSFER_MODE environment variable is set and overrides this setting.";
    }
    BOOL yes;
    if (RequestProcW) {
        WCHAR wtitle[wdirtypemax], wtext[wdirtypemax];
        ws_to_u16buf(wtitle, countof(wtitle), title);
        ws_to_u16buf(wtext, countof(wtext), text);
        yes = RequestProcW(PluginNumber, RT_MsgYesNo, wtitle, wtext, NULL, 0);
    } else if (RequestProc) {
        char atitle[wdirtypemax], atext[wdirtypemax];
        ws_to_utf8buf(atitle, sizeof(atitle), title);
        ws_to_utf8buf(atext, sizeof(atext), text);
        yes = RequestProc(PluginNumber, RT_MsgYesNo, atitle, atext, NULL, 0);
    } else {
        return FS_EXEC_ERROR;
    }
    SaveTransferMode(yes ? TRANSFER_SYNC : TRANSFER_SHELL);
    return FS_EXEC_OK;
}

DCEXPORT int DCPCALL FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    // FS_EXEC_OK = "plugin handled it": the commander stays silent, versus
    // FS_EXEC_ERROR which pops a "Cannot open existing file" box
    if (IsErrorMarker(u16_to_ws(RemoteName)))
        return FS_EXEC_OK;
    // FS_EXEC_YOURSELF makes the commander download the file to its own temp
    // dir, open it with the default application, and clean the copy up itself
    if (u16_to_ws(Verb) == L"open") {
        return FS_EXEC_YOURSELF;
    }
    if (u16_to_ws(Verb) == L"properties") {
        // Double Commander's Configure button targets the plugin root with
        // the native path delimiter; Alt+Enter on files stays unhandled
        wstring rn = u16_to_ws(RemoteName);
        if (rn == L"/" || rn == L"\\")
            return ShowConfigDialog();
    }
    return FS_EXEC_ERROR;
}

DCEXPORT int DCPCALL FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    WCHAR RemoteNameW[wdirtypemax], VerbW[wdirtypemax];
    return FsExecuteFileW(MainWin, awfilenamecopy(RemoteNameW, RemoteName), awfilenamecopy(VerbW, Verb));
}

DCEXPORT int DCPCALL FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    // USERABORT reads as "user cancelled": no error dialog, unlike NOTSUPPORTED
    if (IsErrorMarker(u16_to_ws(OldName)) || IsErrorMarker(u16_to_ws(NewName)))
        return FS_FILE_USERABORT;
    wstring oldq = QuoteString(PathConverter(u16_to_ws(OldName)));
    wstring newq = QuoteString(PathConverter(u16_to_ws(NewName)));
    if (Move) {
        return RunCommand(Tool(L"mv") + L" -f " + oldq + L" " + newq) ? FS_FILE_OK : FS_FILE_WRITEERROR;
    } else {
        return RunCommand(Tool(L"cp") + L" -f " + oldq + L" " + newq) ? FS_FILE_OK : FS_FILE_WRITEERROR;
    }
}

DCEXPORT int DCPCALL FsRenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    WCHAR OldNameW[wdirtypemax], NewNameW[wdirtypemax];
    return FsRenMovFileW(awfilenamecopy(OldNameW, OldName), awfilenamecopy(NewNameW, NewName), Move, OverWrite, ri);
}

DCEXPORT int DCPCALL FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    if (IsErrorMarker(u16_to_ws(RemoteName)))
        return FS_FILE_USERABORT;
    wstring remote = u16_to_ws(RemoteName);
    wstring local = u16_to_ws(LocalName);
    string local8 = ws_to_utf8(local);
    struct stat st;
    bool exists = (stat(local8.c_str(), &st) == 0);
    if (exists && (CopyFlags == 0 || CopyFlags == FS_COPYFLAGS_MOVE)) {
        return FS_FILE_EXISTS;
    }
    if (GetTransferMode() == TRANSFER_SYNC) {
        int64_t fullsize = ((int64_t)ri->SizeHigh << 32) | ri->SizeLow;
        return SyncPull(remote, local, fullsize);
    }
    FILE* f = fopen(local8.c_str(), "wb+");
    if (f == NULL) return FS_FILE_WRITEERROR;
    try {
        // busybox: uuencode -m emits a base64 body; elsewhere plain `base64`
        // emits the same body without the begin/==== framing (which the
        // decode loop below skips anyway)
        wstring cmd = (AdbCommunicator::instance()->ToolMode() == 0)
            ? L"busybox uuencode -m " + QuoteString(PathConverter(remote)) + L" x"
            : Tool(L"base64") + L" " + QuoteString(PathConverter(remote));
        AdbCommunicator::instance()->PushCommandW(cmd);
        string* line = AdbCommunicator::instance()->ReadLine();
        int64_t savedsize = 0;
        int64_t fullsize = ((int64_t)ri->SizeHigh << 32) | ri->SizeLow;
        if (fullsize <= 0) fullsize = 1;
        ProgressT(remote, local, 0);
        int outsize = 0;
        char out[BUF_SIZE * 4];
        while (line != NULL) {
            if (line->find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == string::npos) {
                const char* pos = line->c_str();
                size_t len = line->size();
                for (size_t i = 0; i + 4 <= len; i += 4) {
                    outsize += decode64(pos + i, out + outsize);
                }
            }
            if (outsize > BUF_SIZE * 4 - 128) {
                if ((int)fwrite(out, 1, outsize, f) != outsize) {
                    fclose(f);
                    delete line;
                    return FS_FILE_WRITEERROR;
                }
                savedsize += outsize;
                outsize = 0;
                if (ProgressT(remote, local, (int)((double)savedsize / fullsize * 100))) {
                    fclose(f);
                    delete line;
                    AdbCommunicator::disconnect();
                    return FS_FILE_USERABORT;
                }
            }
            delete line;
            line = AdbCommunicator::instance()->ReadLine();
        }
        if (outsize != 0) {
            if ((int)fwrite(out, 1, outsize, f) != outsize) {
                fclose(f);
                return FS_FILE_WRITEERROR;
            }
        }
        ProgressT(remote, local, 100);
        fclose(f);
        return FS_FILE_OK;
    } catch (wstring&) {
        fclose(f);
        return FS_FILE_READERROR;
    }
}

DCEXPORT int DCPCALL FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    WCHAR RemoteNameW[wdirtypemax], LocalNameW[wdirtypemax];
    return FsGetFileW(awfilenamecopy(RemoteNameW, RemoteName), awfilenamecopy(LocalNameW, LocalName), CopyFlags, ri);
}

DCEXPORT int DCPCALL FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags) {
    if (IsErrorMarker(u16_to_ws(RemoteName)))
        return FS_FILE_USERABORT;
    wstring local = u16_to_ws(LocalName);
    wstring remote = u16_to_ws(RemoteName);
    if (GetTransferMode() == TRANSFER_SYNC) {
        return SyncPush(local, remote);
    }
    string local8 = ws_to_utf8(local);
    FILE* f = fopen(local8.c_str(), "rb");
    if (f == NULL) {
        return FS_FILE_READERROR;
    }
    try {
        // busybox: uudecode with begin-base64 framing; elsewhere `base64 -d`
        // reading the raw stream from the pty until EOF (^D)
        bool uumode = (AdbCommunicator::instance()->ToolMode() == 0);
        if (uumode) {
            AdbCommunicator::instance()->PushCommandW(L"busybox uudecode -o " + QuoteString(PathConverter(remote)));
        } else {
            AdbCommunicator::instance()->PushCommandW(Tool(L"base64") + L" -d > " + QuoteString(PathConverter(remote)));
        }
        ProgressT(local, remote, 0);

        if (uumode) {
            AdbCommunicator::instance()->PutData("begin-base64 644 x\n", 19);
        }

        struct stat st;
        int64_t fullsize = (stat(local8.c_str(), &st) == 0) ? (int64_t)st.st_size : 0;
        if (fullsize <= 0) fullsize = 1;
        int64_t savedsize = 0;

        char buf[45];
        int read = (int)fread(buf, 1, 45, f);
        char out[61];
        out[60] = '\n';
        while (read == 45) {
            int outwr = 0;
            int inr = 0;
            while (inr != 45) {
                encode64(buf + inr, out + outwr);
                outwr += 4;
                inr += 3;
            }
            AdbCommunicator::instance()->PutData(out, 61);
            AdbCommunicator::instance()->CleanBuffer(false);
            read = (int)fread(buf, 1, 45, f);
            savedsize += inr;
            if (ProgressT(local, remote, (int)((double)savedsize / fullsize * 100))) {
                fclose(f);
                AdbCommunicator::disconnect();
                return FS_FILE_USERABORT;
            }
        }
        if (read > 0) {
            int outwr = 0;
            int inr = 0;
            while (read > 2) {
                encode64(buf + inr, out + outwr);
                inr += 3;
                outwr += 4;
                read -= 3;
            }
            if (read == 2) {
                buf[inr + 2] = 0;
                encode64(buf + inr, out + outwr);
                outwr += 4;
                out[outwr - 1] = '=';
                out[outwr] = '\n';
            } else if (read == 1) {
                buf[inr + 1] = 0;
                buf[inr + 2] = 0;
                encode64(buf + inr, out + outwr);
                outwr += 4;
                out[outwr - 2] = '=';
                out[outwr - 1] = '=';
                out[outwr] = '\n';
            } else {
                out[outwr] = '\n';   // upstream bug: sent an uninitialized byte when size % 3 == 0
            }
            AdbCommunicator::instance()->PutData(out, outwr + 1);
        }
        if (uumode) {
            AdbCommunicator::instance()->PutData("====\x04\n", 6);
        } else {
            // ^D at line start ends the pty input stream for `base64 -d`
            AdbCommunicator::instance()->PutData("\x04\n", 2);
        }
        Sleep(100);

        ProgressT(local, remote, 100);
        fclose(f);
    } catch (wstring&) {
        fclose(f);
        return FS_FILE_WRITEERROR;
    }
    return FS_FILE_OK;
}

DCEXPORT int DCPCALL FsPutFile(char* LocalName, char* RemoteName, int CopyFlags) {
    WCHAR LocalNameW[wdirtypemax], RemoteNameW[wdirtypemax];
    return FsPutFileW(awfilenamecopy(LocalNameW, LocalName), awfilenamecopy(RemoteNameW, RemoteName), CopyFlags);
}

DCEXPORT BOOL DCPCALL FsDeleteFileW(WCHAR* RemoteName) {
    // claim success: nothing to delete, and returning FALSE pops an error box
    if (IsErrorMarker(u16_to_ws(RemoteName)))
        return 1;
    return RunCommand(Tool(L"rm") + L" " + QuoteString(PathConverter(u16_to_ws(RemoteName))));
}

DCEXPORT BOOL DCPCALL FsDeleteFile(char* RemoteName) {
    WCHAR RemoteNameW[wdirtypemax];
    return FsDeleteFileW(awfilenamecopy(RemoteNameW, RemoteName));
}

DCEXPORT BOOL DCPCALL FsRemoveDirW(WCHAR* RemoteName) {
    if (u16len(RemoteName) < pluginrootlen + 2)
        return 0;
    if (IsErrorMarker(u16_to_ws(RemoteName)))
        return 1;
    return RunCommand(L"rm -r " + QuoteString(PathConverter(u16_to_ws(RemoteName))));
}

DCEXPORT BOOL DCPCALL FsRemoveDir(char* RemoteName) {
    WCHAR RemoteNameW[wdirtypemax];
    return FsRemoveDirW(awfilenamecopy(RemoteNameW, RemoteName));
}

DCEXPORT BOOL DCPCALL FsSetAttrW(WCHAR* RemoteName, int NewAttr) {
    // Windows file attributes don't map to the device; unsupported
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetAttr(char* RemoteName, int NewAttr) {
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetTimeW(WCHAR* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime, FILETIME* LastWriteTime) {
    return 0;
}

DCEXPORT BOOL DCPCALL FsSetTime(char* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime, FILETIME* LastWriteTime) {
    return 0;
}

DCEXPORT void DCPCALL FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation) {
}

DCEXPORT void DCPCALL FsGetDefRootName(char* DefRootName, int maxlen) {
    snprintf(DefRootName, maxlen, "%s", "Android");
}

DCEXPORT int DCPCALL FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, PWfxIcon TheIcon) {
    return FS_ICON_USEDEFAULT;
}

DCEXPORT int DCPCALL FsExtractCustomIcon(char* RemoteName, int ExtractFlags, PWfxIcon TheIcon) {
    return FS_ICON_USEDEFAULT;
}

DCEXPORT int DCPCALL FsGetPreviewBitmap(char* RemoteName, int width, int height, HBITMAP* ReturnedBitmap) {
    return FS_BITMAP_NONE;
}

DCEXPORT int DCPCALL FsGetPreviewBitmapW(WCHAR* RemoteName, int width, int height, HBITMAP* ReturnedBitmap) {
    return FS_BITMAP_NONE;
}

DCEXPORT void DCPCALL FsSetDefaultParams(FsDefaultParamStruct* dps) {
    SetConfigIniPath(dps->DefaultIniName);
}

/**************************************************************************************/
/*********************** content plugin = custom columns part! ************************/
/**************************************************************************************/

#define fieldcount 5
static const char* fieldnames[fieldcount] = {"mode", "uid", "gid", "type", "name"};
static int fieldtypes[fieldcount] = {ft_string, ft_numeric_32, ft_numeric_32, ft_string, ft_string};
static const char* fieldunits_and_multiplechoicestrings[fieldcount] = {"", "", "", "", ""};
static int fieldflags[fieldcount] = {0, 0, 0, 0, 0};
static int sortorders[fieldcount] = {-1, -1, -1, -1, -1};

DCEXPORT int DCPCALL FsContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen) {
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return ft_nomorefields;
    snprintf(FieldName, maxlen, "%s", fieldnames[FieldIndex]);
    snprintf(Units, maxlen, "%s", fieldunits_and_multiplechoicestrings[FieldIndex]);
    return fieldtypes[FieldIndex];
}

static int ContentGetValue(const wstring& path, int FieldIndex, void* FieldValue, int maxlen) {
    auto it = cacheMap.find(path);
    if (it == cacheMap.end())
        return ft_fileerror;
    FileData* fd = &it->second;
    switch (FieldIndex) {
    case 0: {
        char* text = (char*)FieldValue;
        if (maxlen < 12) return ft_fileerror;
        snprintf(text, maxlen, "%s", "--- --- ---");
        if (fd->mode & 0400) { text[0] = 'r'; }
        if (fd->mode & 0200) { text[1] = 'w'; }
        if (fd->mode & 0100) { text[2] = 'x'; }
        if (fd->mode & 040) { text[4] = 'r'; }
        if (fd->mode & 020) { text[5] = 'w'; }
        if (fd->mode & 010) { text[6] = 'x'; }
        if (fd->mode & 04) { text[8] = 'r'; }
        if (fd->mode & 02) { text[9] = 'w'; }
        if (fd->mode & 01) { text[10] = 'x'; }
        break;
    }
    case 1:
        *(int*)FieldValue = (int)fd->uid;
        break;
    case 2:
        *(int*)FieldValue = (int)fd->gid;
        break;
    case 3: {
        char* text = (char*)FieldValue;
        if (fd->islink) snprintf(text, maxlen, "%s", "link");
        else if (fd->type == REGFILE) snprintf(text, maxlen, "%s", "file");
        else if (fd->type == DIRECTORY) snprintf(text, maxlen, "%s", "dir");
        else if (fd->type == LINK) snprintf(text, maxlen, "%s", "link");
        else snprintf(text, maxlen, "%s", "other");
        break;
    }
    case 4:
        ws_to_utf8buf((char*)FieldValue, maxlen, fd->alt_name);
        break;
    default:
        return ft_nosuchfield;
    }
    return fieldtypes[FieldIndex];  // very important!
}

DCEXPORT int DCPCALL FsContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    return ContentGetValue(PathConverter(u16_to_ws(FileName)), FieldIndex, FieldValue, maxlen);
}

DCEXPORT int DCPCALL FsContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags) {
    return ContentGetValue(PathConverter(utf8_to_ws(FileName)), FieldIndex, FieldValue, maxlen);
}

DCEXPORT int DCPCALL FsContentGetSupportedFieldFlags(int FieldIndex) {
    if (FieldIndex == -1)
        return contflags_substmask | contflags_edit;
    else if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 0;
    else
        return fieldflags[FieldIndex];
}

DCEXPORT int DCPCALL FsContentGetDefaultSortOrder(int FieldIndex) {
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 1;
    else
        return sortorders[FieldIndex];
}

DCEXPORT BOOL DCPCALL FsContentGetDefaultView(char* ViewContents, char* ViewHeaders, char* ViewWidths, char* ViewOptions, int maxlen) {
    // separated by backslash and n, not new lines!
    snprintf(ViewContents, maxlen, "%s", "[=tc.size]\\n[=<fs>.mode]\\n[=<fs>.uid]\\n[=<fs>.gid]\\n[=<fs>.type]\\n[=<fs>.name]");
    snprintf(ViewHeaders, maxlen, "%s", "size\\nmode\\nuid\\ngid\\ntype\\nname");
    snprintf(ViewWidths, maxlen, "%s", "148,23,-35,40,-18,-18,16,148");
    snprintf(ViewOptions, maxlen, "%s", "-1|0");
    return 1;
}

DCEXPORT int DCPCALL FsContentSetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, int FieldType, void* FieldValue, int flags) {
    return ft_fileerror;
}

DCEXPORT int DCPCALL FsContentSetValue(char* FileName, int FieldIndex, int UnitIndex, int FieldType, void* FieldValue, int flags) {
    return ft_fileerror;
}

DCEXPORT void DCPCALL FsContentPluginUnloading(void) {
    AdbCommunicator::disconnect();
}

DCEXPORT BOOL DCPCALL FsDisconnect(char* DisconnectRoot) {
    AdbCommunicator::disconnect();
    return 1;
}

DCEXPORT BOOL DCPCALL FsDisconnectW(WCHAR* DisconnectRoot) {
    AdbCommunicator::disconnect();
    return 1;
}
