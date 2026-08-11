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

FakeAdbServer::FakeAdbServer(bool stock) : stock_(stock) {
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

void FakeAdbServer::run() {
    while (!stop_) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        rbuf_.clear();
        serveConnection(fd);
        close(fd);
    }
}

void FakeAdbServer::sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// smart-socket phase: <4 hex chars length><payload>
void FakeAdbServer::serveConnection(int fd) {
    for (;;) {
        char lenbuf[5] = {0};
        ssize_t n = recv(fd, lenbuf, 4, MSG_WAITALL);
        if (n != 4) return;
        int msglen = (int)strtol(lenbuf, nullptr, 16);
        std::string payload(msglen, 0);
        if (recv(fd, &payload[0], msglen, MSG_WAITALL) != msglen) return;
        if (payload == "host:transport-usb") {
            sendAll(fd, "OKAY");
        } else if (payload == "shell:") {
            sendAll(fd, "OKAY");
            shellSession(fd);
            return;
        } else {
            sendAll(fd, "FAIL0013unknown fake request");
            return;
        }
    }
}

bool FakeAdbServer::readLine(int fd, std::string* line) {
    for (;;) {
        size_t nl = rbuf_.find('\n');
        if (nl != std::string::npos) {
            *line = rbuf_.substr(0, nl);
            rbuf_.erase(0, nl + 1);
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rbuf_.append(buf, (size_t)n);
    }
}

void FakeAdbServer::shellSession(int fd) {
    std::string line;
    while (readLine(fd, &line)) {
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
        handleShellCommand(fd, cmd);
        sendAll(fd, "===adbfsplugin-->\n");
    }
}

void FakeAdbServer::handleShellCommand(int fd, const std::string& cmd) {
    if (stock_) {
        if (cmd.rfind("busybox", 0) == 0) {
            sendAll(fd, "/system/bin/sh: busybox: inaccessible or not found\n");
        } else if (cmd == "toybox echo adbfsprobe") {
            sendAll(fd, "adbfsprobe\n");
        } else if (cmd.rfind("toybox ls ", 0) == 0) {
            sendAll(fd, "file one\nsubdir\nlink1\n\xF0\x9F\x98\x80.txt\n");
        } else if (cmd.rfind("toybox stat ", 0) == 0) {
            auto args = quotedArgs(cmd);
            std::string out;
            for (auto& a : args) {
                if (a.find('%') != std::string::npos) continue;
                std::string base = basenameOf(a);
                if (base == "subdir")
                    out += "755 -directory- 0 0 4096 1600000000 1600000100 1600000200 " + a + "\n";
                else if (base == "link1")
                    out += "777 -symbolic link- 0 0 11 1600000000 1600000100 1600000200 " + a + "\n";
                else
                    out += "644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 " + a + "\n";
            }
            sendAll(fd, out);
        } else if (cmd.rfind("toybox base64 -d > ", 0) == 0) {
            std::string data, l;
            while (readLine(fd, &l)) {
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
        auto args = quotedArgs(cmd);
        std::string out;
        for (auto& a : args) {
            if (a.find('%') != std::string::npos) continue;   // the -c format argument
            std::string base = basenameOf(a);
            if (base == "subdir")
                out += "755 -directory- 0 0 4096 1600000000 1600000100 1600000200 '" + a + "'\n";
            else if (base == "link1")
                out += "777 -symbolic link- 0 0 11 1600000000 1600000100 1600000200 '" + a + "' -> '/target'\n";
            else
                out += "644 -regular file- 1000 2000 12 1700000000 1700000001 1700000002 '" + a + "'\n";
        }
        sendAll(fd, out);
    } else if (cmd.rfind("busybox uuencode ", 0) == 0) {
        sendAll(fd, "begin-base64 644 x\n" + base64(kFileContent) + "\n====\n");
    } else if (cmd.rfind("busybox uudecode ", 0) == 0) {
        // consume the in-band upload until the plugin's EOT marker
        std::string data, l;
        while (readLine(fd, &l)) {
            if (l == "====\x04") break;
            data += l;
            data += '\n';
        }
        std::lock_guard<std::mutex> lk(mu_);
        uploaded_ = data;
    }
    // mkdir/rm/mv/cp: recorded in commands_, empty output
}
