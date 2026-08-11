#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal in-process stand-in for the local ADB server plus a device shell.
// Speaks just enough of the smart-socket protocol for adbfsplugin:
//   <4-hex-len><payload> requests, OKAY responses, then a line-based shell.
class FakeAdbServer {
public:
    // stock=true emulates a modern stock Android device: no busybox
    // (commands fail with "inaccessible or not found"), toybox applets
    // (ls/stat/mkdir/base64) available instead.
    // wireless=true emulates a device connected over TCP only: the USB-only
    // selector host:transport-usb FAILs ("no devices found", as the real
    // server answers), every other transport request succeeds.
    explicit FakeAdbServer(bool stock = false, bool wireless = false);
    ~FakeAdbServer();
    int port() const { return port_; }
    std::vector<std::string> commands();   // device shell commands, marker framing stripped
    std::string uploaded();                // raw lines captured after a uudecode command
    std::vector<std::string> transports(); // host:transport* requests received

private:
    void run();
    void serveConnection(int fd);
    void shellSession(int fd);
    void handleShellCommand(int fd, const std::string& cmd);
    bool readLine(int fd, std::string* line);
    static void sendAll(int fd, const std::string& data);

    bool stock_ = false;
    bool wireless_ = false;
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::string> commands_;
    std::vector<std::string> transports_;
    std::string uploaded_;
    std::string rbuf_;                     // connection read buffer
};
