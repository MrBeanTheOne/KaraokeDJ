#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "library/media_query.h"

namespace httplib { class Server; }
class Db;

// A song request submitted from a phone.
struct PhoneRequest {
    std::wstring singer;
    Match song; // full row snapshot — approving needs no lookup
};

// Optional embedded LAN web server (settings toggle, off by default):
// singers on the venue Wi-Fi open one mobile page, search the library and
// request songs. Fully isolated from the playback path — it runs on its own
// threads with its own read-only db connection, and requests land in a
// mutex-guarded inbox the app's main loop drains. When off, nothing here
// exists: no thread, no socket, no cost.
class RequestServer {
public:
    bool start(const std::wstring& dbPath, int firstPort = 8080);
    void stop();
    bool running() const { return running_.load(); }
    std::wstring url() const; // http://<lan-ip>:<port>/  ("" if not running)

    bool hasPending() const { return pending_.load(); }
    std::vector<PhoneRequest> take(); // drain the inbox (main loop)

    // Both defined in the .cpp: inline versions would need the complete
    // httplib::Server type in every including TU.
    RequestServer();
    ~RequestServer();

private:
    std::unique_ptr<httplib::Server> srv_;
    std::unique_ptr<Db> db_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> pending_{false};
    int port_ = 0;

    std::mutex mx_;
    std::vector<PhoneRequest> inbox_;
};
