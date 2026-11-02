#ifndef AC_API_V1_HTTP_SERVER_H
#define AC_API_V1_HTTP_SERVER_H

#include "ApiService.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ac_api_v1_httplib
{
class Server;
struct Request;
struct Response;
}

namespace AcApiV1
{
struct HttpServerConfig
{
    std::string bindAddress = "127.0.0.1";
    int port = 8090;
    int workerThreads = 4;
    int maxQueuedRequests = 64;
    int timeoutSeconds = 5;
    int requestsPerMinute = 120;
    std::size_t maxBodyBytes = 16384;
};

class HttpServer
{
public:
    HttpServer(HttpServerConfig config, ApiService& service);
    ~HttpServer();

    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    bool AllowRequest(ac_api_v1_httplib::Request const& request);
    void Handle(ac_api_v1_httplib::Request const& request,
        ac_api_v1_httplib::Response& response);
    void Write(Response const& source, ac_api_v1_httplib::Response& target) const;

    HttpServerConfig _config;
    ApiService& _service;
    std::unique_ptr<ac_api_v1_httplib::Server> _server;
    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _failed{false};
    std::mutex _rateMutex;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> _requests;
};
}

#endif
