#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal in-process stand-in for the local ADB server plus a device shell.
// Speaks just enough of the smart-socket protocol for adbfsplugin:
//   <4-hex-len><payload> requests, OKAY responses, then a line-based shell
//   or the binary sync service (what adb pull/push uses).
// Each accepted connection gets its own thread: the plugin keeps its shell
// connection open while sync transfers run on separate connections.
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
    std::vector<std::string> syncRequests(); // "RECV <path>" / "SEND <path>,<mode>"
    std::string syncUploaded();            // bytes received through sync SEND
    void dropConnection();                 // kill the active connection (device vanished)
    void goSilent();                       // device stops answering but the TCP link stays up

private:
    void run();
    void serveConnection(int fd);
    void shellSession(int fd, std::string* rbuf);
    void handleShellCommand(int fd, const std::string& cmd, std::string* rbuf);
    void syncSession(int fd);
    bool readLine(int fd, std::string* line, std::string* rbuf);
    static bool readN(int fd, void* buf, size_t n);
    static void sendAll(int fd, const std::string& data);
    static void sendSyncFrame(int fd, const char* id, const std::string& payload);
    static void sendSyncHeader(int fd, const char* id, unsigned len);

    bool stock_ = false;
    bool wireless_ = false;
    int listen_fd_ = -1;
    std::atomic<int> conn_fd_{-1};
    std::atomic<bool> silent_{false};
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::string> commands_;
    std::vector<std::string> transports_;
    std::vector<std::string> sync_requests_;
    std::string sync_uploaded_;
    std::string uploaded_;
    std::vector<int> open_fds_;            // for unblocking workers in the destructor
};
