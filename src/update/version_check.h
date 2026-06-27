// Background update check: GETs the version endpoint, compares the published
// version to the local one, and reports whether an update is available.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class VersionCheck
{
public:
    enum State { Idle, Checking, UpToDate, UpdateAvailable, Unreleased, Error };

    ~VersionCheck();

    // Kick off the check once (no-op if already started).
    void start(const std::string& slug, const std::string& localVersion);

    State state() const { return state_.load(); }
    std::string latestVersion();
    std::string productUrl();
    std::string error();

private:
    void run(std::string slug, std::string localVersion);

    std::atomic<State> state_{Idle};
    std::atomic<bool> started_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::string latest_, url_, err_;
};
