// Embedded HTTP server + REST API serving the web dashboard.
#pragma once

#include <atomic>
#include <string>
#include <thread>

class DecoderManager;
class SdrSource;

class WebServer
{
public:
    ~WebServer() { stop(); }

    void start(int port);
    void stop();
    bool running() const { return running_.load(); }

    // Data providers set by main thread before start.
    DecoderManager* decodersA = nullptr;
    DecoderManager* decodersB = nullptr;
    bool*           dualMode = nullptr;
    SdrSource**     active = nullptr;

private:
    void serve(int port);
    std::thread thread_;
    std::atomic<bool> running_{false};
};
