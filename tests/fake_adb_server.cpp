#include "fake_adb_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

namespace {

std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned n = (unsigned char)in[i] << 16;
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        unsigned n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += tbl[n >> 18]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += '=';
    }
    return out;
}

// paths appear as '...'-quoted arguments; names in the tests contain no quotes
std::vector<std::string> quotedArgs(const std::string& cmd) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t a = cmd.find('\'', pos);
        if (a == std::string::npos) break;
        size_t b = cmd.find('\'', a + 1);
        if (b == std::string::npos) break;
        out.push_back(cmd.substr(a + 1, b - a - 1));
        pos = b + 1;
    }
    return out;
}

std::string basenameOf(const std::string& path) {
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

const char kFileContent[] = "hello adbfs!";

} // namespace

FakeAdbServer::FakeAdbServer(bool stock, bool wireless) : stock_(stock), wireless_(wireless) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listen_fd_, (sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, (sockaddr*)&addr, &len);
    port_ = ntohs(addr.sin_port);
    listen(listen_fd_, 4);
    thread_ = std::thread([this] { run(); });
}

FakeAdbServer::~FakeAdbServer() {
    stop_ = true;
    if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); }
    {
        // unblock any worker still stuck in recv on a live connection
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : open_fds_) shutdown(fd, SHUT_RDWR);
    }
    if (thread_.joinable()) thread_.join();
}

std::vector<std::string> FakeAdbServer::commands() {
    std::lock_guard<std::mutex> lk(mu_);
    return commands_;
}

std::string FakeAdbServer::uploaded() {
    std::lock_guard<std::mutex> lk(mu_);
    return uploaded_;
}

std::vector<std::string> FakeAdbServer::transports() {
    std::lock_guard<std::mutex> lk(mu_);
    return transports_;
}

std::vector<std::string> FakeAdbServer::syncRequests() {
    std::lock_guard<std::mutex> lk(mu_);
    return sync_requests_;
}

std::string FakeAdbServer::syncUploaded() {
    std::lock_guard<std::mutex> lk(mu_);
    return sync_uploaded_;
}

void FakeAdbServer::run() {
    std::vector<std::thread> workers;
    while (!stop_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break;
        conn_fd_ = fd;
        {
            std::lock_guard<std::mutex> lk(mu_);
            open_fds_.push_back(fd);
        }
        workers.emplace_back([this, fd] {
            serveConnection(fd);
            int expected = fd;
            conn_fd_.compare_exchange_strong(expected, -1);   // clear only if still current
            {
                // unregister before close: the destructor must never shut
                // down an fd number this thread has already released
                std::lock_guard<std::mutex> lk(mu_);
                for (auto it = open_fds_.begin(); it != open_fds_.end(); ++it)
                    if (*it == fd) { open_fds_.erase(it); break; }
            }
            close(fd);
        });
    }
    for (auto& w : workers) w.join();
}

void FakeAdbServer::dropConnection() {
    int fd = conn_fd_.exchange(-1);
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

void FakeAdbServer::goSilent() {
    silent_ = true;
}

void FakeAdbServer::sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

bool FakeAdbServer::readN(int fd, void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, (char*)buf + off, n - off, 0);
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

// smart-socket phase: <4 hex chars length><payload>
void FakeAdbServer::serveConnection(int fd) {
    std::string rbuf;
    for (;;) {
        char lenbuf[5] = {0};
        ssize_t n = recv(fd, lenbuf, 4, MSG_WAITALL);
        if (n != 4) return;
        int msglen = (int)strtol(lenbuf, nullptr, 16);
        std::string payload(msglen, 0);
        if (recv(fd, &payload[0], msglen, MSG_WAITALL) != msglen) return;
        if (payload.rfind("host:transport", 0) == 0) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                transports_.push_back(payload);
            }
            if (wireless_ && payload == "host:transport-usb") {
                sendAll(fd, "FAIL0010no devices found");
                return;
            }
            sendAll(fd, "OKAY");
        } else if (payload == "shell:") {
            sendAll(fd, "OKAY");
            shellSession(fd, &rbuf);
            return;
        } else if (payload == "sync:") {
            sendAll(fd, "OKAY");
            syncSession(fd);
            return;
        } else {
            sendAll(fd, "FAIL0013unknown fake request");
            return;
        }
    }
}

// binary sync-service frame: 4-byte id + little-endian uint32 length
void FakeAdbServer::sendSyncHeader(int fd, const char* id, unsigned len) {
    char hdr[8];
    memcpy(hdr, id, 4);
    memcpy(hdr + 4, &len, 4);
    sendAll(fd, std::string(hdr, 8));
}

void FakeAdbServer::sendSyncFrame(int fd, const char* id, const std::string& payload) {
    sendSyncHeader(fd, id, (unsigned)payload.size());
    sendAll(fd, payload);
}

void FakeAdbServer::syncSession(int fd) {
    for (;;) {
        char id[4];
        unsigned len = 0;
        if (!readN(fd, id, 4) || !readN(fd, &len, 4)) return;
        std::string sid(id, 4);
        if (sid == "QUIT") return;
        std::string arg(len, 0);
        if (len && !readN(fd, &arg[0], len)) return;
        if (sid == "RECV") {
            {
                std::lock_guard<std::mutex> lk(mu_);
                sync_requests_.push_back("RECV " + arg);
            }
            if (arg.find("denied") != std::string::npos) {
                sendSyncFrame(fd, "FAIL", "Permission denied");
                return;
            }
            // two DATA frames so the client's reassembly is exercised
            sendSyncFrame(fd, "DATA", "hello ");
            sendSyncFrame(fd, "DATA", "adbfs!");
            sendSyncHeader(fd, "DONE", 1700000001u);
        } else if (sid == "SEND") {
            {
                std::lock_guard<std::mutex> lk(mu_);
                sync_requests_.push_back("SEND " + arg);
            }
            if (arg.find("stall") != std::string::npos) {
                // wedged device: accepts the request, then never reads the
                // data — the client's socket buffer fills and send() blocks
                while (!stop_) usleep(50000);
                return;
            }
            std::string data;
            for (;;) {
                char cid[4];
                unsigned clen = 0;
                if (!readN(fd, cid, 4) || !readN(fd, &clen, 4)) return;
                std::string scid(cid, 4);
                if (scid == "DONE") break;   // clen carries the mtime
                if (scid != "DATA") return;
                std::string chunk(clen, 0);
                if (clen && !readN(fd, &chunk[0], clen)) return;
                data += chunk;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                sync_uploaded_ = data;
            }
            if (arg.find("denied") != std::string::npos)
                sendSyncFrame(fd, "FAIL", "Permission denied");
            else
                sendSyncHeader(fd, "OKAY", 0);
        } else {
            return;
        }
    }
}

bool FakeAdbServer::readLine(int fd, std::string* line, std::string* rbuf) {
    for (;;) {
        size_t nl = rbuf->find('\n');
        if (nl != std::string::npos) {
            *line = rbuf->substr(0, nl);
            rbuf->erase(0, nl + 1);
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rbuf->append(buf, (size_t)n);
    }
}

void FakeAdbServer::shellSession(int fd, std::string* rbuf) {
    std::string line;
    while (readLine(fd, &line, rbuf)) {
        if (silent_) continue;   // wedged device: reads everything, answers nothing
        if (line == "su") {
            sendAll(fd, "# ");
            continue;
        }
        const std::string prefix = "echo \"===adbfsplugin<--\" ;";
        const std::string suffix = " ; echo \"===adbfsplugin-->\"";
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string cmd = line.substr(prefix.size());
        size_t tail = cmd.rfind(suffix);
        if (tail != std::string::npos) cmd.erase(tail);
        while (!cmd.empty() && cmd.front() == ' ') cmd.erase(0, 1);
        {
            std::lock_guard<std::mutex> lk(mu_);
            commands_.push_back(cmd);
        }
        sendAll(fd, line + "\r\n");             // pty echo, discarded by the plugin
        sendAll(fd, "===adbfsplugin<--\n");
        handleShellCommand(fd, cmd, rbuf);
        sendAll(fd, "===adbfsplugin-->\n");
    }
}

void FakeAdbServer::handleShellCommand(int fd, const std::string& cmd, std::string* rbuf) {
    if (stock_) {
        if (cmd.rfind("busybox", 0) == 0) {
            sendAll(fd, "/system/bin/sh: busybox: inaccessible or not found\n");
        } else if (cmd == "toybox echo adbfsprobe") {
            sendAll(fd, "adbfsprobe\n");
        } else if (cmd.rfind("toybox ls ", 0) == 0) {
            // on a pty, stock toybox ls colorizes AND backslash-escapes
            // spaces; piped through cat it prints plain names
            bool piped = cmd.size() >= 6 && cmd.compare(cmd.size() - 6, 6, " | cat") == 0;
            if (piped)
                sendAll(fd, "file one\nsubdir\nlink1\n\xF0\x9F\x98\x80.txt\n");
            else
                sendAll(fd, "file\\ one\n\x1B[1;34msubdir\x1B[0m\n\x1B[1;36mlink1\x1B[0m\n\xF0\x9F\x98\x80.txt\n");
        } else if (cmd.rfind("toybox stat ", 0) == 0) {
            // link1 models /sdcard: a symlink whose target is a directory,
            // visible only when stat is asked to follow (-L)
            bool follow = cmd.find(" -L ") != std::string::npos;
            auto args = quotedArgs(cmd);
            std::string out;
            for (auto& a : args) {
                if (a.find('%') != std::string::npos) continue;
                std::string base = basenameOf(a);
                if (base == "subdir")
                    out += "755 -directory- 0 0 4096 1600000000 1600000100 1600000200 " + a + "\n";
                else if (base == "link1")
                    out += follow
                        ? "771 -directory- 9997 0 4096 1600000000 1600000100 1600000200 " + a + "\n"
                        : "777 -symbolic link- 0 0 11 1600000000 1600000100 1600000200 " + a + "\n";
                else
                    out += "644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 " + a + "\n";
            }
            sendAll(fd, out);
        } else if (cmd.rfind("toybox base64 -d > ", 0) == 0) {
            std::string data, l;
            while (readLine(fd, &l, rbuf)) {
                if (l == "\x04") break;
                data += l;
                data += '\n';
            }
            std::lock_guard<std::mutex> lk(mu_);
            uploaded_ = data;
        } else if (cmd.rfind("toybox base64 ", 0) == 0) {
            sendAll(fd, base64(kFileContent) + "\n");
        }
        // toybox mkdir/mv/cp/rm: recorded in commands_, empty output
        return;
    }
    if (cmd == "busybox echo adbfsprobe") {
        sendAll(fd, "adbfsprobe\n");
    } else if (cmd.rfind("busybox ls ", 0) == 0) {
        sendAll(fd, "file one\nsubdir\nlink1\n\xF0\x9F\x98\x80.txt\n");
    } else if (cmd.rfind("busybox stat ", 0) == 0) {
        bool follow = cmd.find(" -L ") != std::string::npos;
        auto args = quotedArgs(cmd);
        std::string out;
        for (auto& a : args) {
            if (a.find('%') != std::string::npos) continue;   // the -c format argument
            std::string base = basenameOf(a);
            if (base == "subdir")
                out += "755 -directory- 0 0 4096 1600000000 1600000100 1600000200 '" + a + "'\n";
            else if (base == "link1")
                out += follow
                    ? "771 -directory- 9997 0 4096 1600000000 1600000100 1600000200 '" + a + "'\n"
                    : "777 -symbolic link- 0 0 11 1600000000 1600000100 1600000200 '" + a + "' -> '/target'\n";
            else
                out += "644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 '" + a + "'\n";
        }
        sendAll(fd, out);
    } else if (cmd.rfind("busybox uuencode ", 0) == 0) {
        sendAll(fd, "begin-base64 644 x\n" + base64(kFileContent) + "\n====\n");
    } else if (cmd.rfind("busybox uudecode ", 0) == 0) {
        // consume the in-band upload until the plugin's EOT marker
        std::string data, l;
        while (readLine(fd, &l, rbuf)) {
            if (l == "====\x04") break;
            data += l;
            data += '\n';
        }
        std::lock_guard<std::mutex> lk(mu_);
        uploaded_ = data;
    }
    // mkdir/rm/mv/cp: recorded in commands_, empty output
}
