#define CPPHTTPLIB_REQUEST_URI_MAX_LENGTH 2048
#define CPPHTTPLIB_HEADER_MAX_LENGTH 8192
#define CPPHTTPLIB_HEADER_MAX_COUNT 64

#include "HttpServer.h"

#include "../deps/cpp-httplib/httplib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace AcApiV1
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
}

HttpServer::HttpServer(HttpServerConfig config, ApiService& service)
    : _config(std::move(config)), _service(service),
      _server(std::make_unique<ac_api_v1_httplib::Server>())
{
    _server->set_payload_max_length(_config.maxBodyBytes);
    _server->set_read_timeout(_config.timeoutSeconds);
    _server->set_write_timeout(_config.timeoutSeconds);
    _server->set_keep_alive_timeout(_config.timeoutSeconds);
    _server->set_keep_alive_max_count(20);
    _server->new_task_queue = [threads = _config.workerThreads, queued = _config.maxQueuedRequests]
    {
        return new ac_api_v1_httplib::ThreadPool(static_cast<std::size_t>(threads),
            static_cast<std::size_t>(threads), static_cast<std::size_t>(queued));
    };

    _server->set_start_handler([this]
    {
        _running = true;
    });

    _server->set_exception_handler([this](ac_api_v1_httplib::Request const&,
                                       ac_api_v1_httplib::Response& response,
                                       std::exception_ptr)
    {
        Write(Error(503, "backend_unavailable",
            "The request could not be completed by the AzerothCore backend."), response);
    });

    auto handler = [this](ac_api_v1_httplib::Request const& request,
                       ac_api_v1_httplib::Response& response)
    {
        Handle(request, response);
    };
    _server->Get(R"(/.*)", handler);
    _server->Post(R"(/.*)", handler);
    _server->Put(R"(/.*)", handler);
    _server->Patch(R"(/.*)", handler);
    _server->Delete(R"(/.*)", handler);
    _server->Options(R"(/.*)", handler);
}

HttpServer::~HttpServer()
{
    Stop();
}

bool HttpServer::Start()
{
    if (_thread.joinable())
        return _running;

    _failed = false;
    _thread = std::thread([this]
    {
        if (!_server->listen(_config.bindAddress, _config.port))
            _failed = true;
        _running = false;
    });

    for (int attempt = 0; attempt < 100 && !_running && !_failed; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!_running)
    {
        Stop();
        return false;
    }

    return true;
}

void HttpServer::Stop()
{
    if (_server)
        _server->stop();
    if (_thread.joinable())
        _thread.join();
    _running = false;
}

bool HttpServer::IsRunning() const
{
    return _running;
}

bool HttpServer::AllowRequest(ac_api_v1_httplib::Request const& request)
{
    if (request.path == "/healthz" || request.path == "/readyz")
        return true;

    auto const now = std::chrono::steady_clock::now();
    auto const cutoff = now - std::chrono::minutes(1);
    std::string const key = request.remote_addr;

    std::lock_guard lock(_rateMutex);
    auto& timestamps = _requests[key];
    while (!timestamps.empty() && timestamps.front() <= cutoff)
        timestamps.pop_front();

    if (timestamps.size() >= static_cast<std::size_t>(_config.requestsPerMinute))
        return false;

    timestamps.push_back(now);
    if (_requests.size() > 4096)
    {
        for (auto iterator = _requests.begin(); iterator != _requests.end();)
        {
            auto& values = iterator->second;
            while (!values.empty() && values.front() <= cutoff)
                values.pop_front();
            if (values.empty())
                iterator = _requests.erase(iterator);
            else
                ++iterator;
        }
    }
    return true;
}

void HttpServer::Handle(ac_api_v1_httplib::Request const& source,
    ac_api_v1_httplib::Response& target)
{
    if (!AllowRequest(source))
    {
        target.set_header("Retry-After", "60");
        Write(Error(429, "rate_limited", "The request rate limit has been exceeded."), target);
        return;
    }

    Request request;
    request.method = source.method;
    request.path = source.path;
    request.body = source.body;
    for (auto const& [name, value] : source.headers)
        request.headers.emplace(Lower(name), value);
    for (auto const& [name, value] : source.params)
        request.query.emplace(name, value);

    Write(_service.Handle(request), target);
}

void HttpServer::Write(Response const& source, ac_api_v1_httplib::Response& target) const
{
    target.status = source.status;
    target.set_header("Cache-Control", "no-store");
    target.set_header("X-Content-Type-Options", "nosniff");
    if (source.status == 401)
        target.set_header("WWW-Authenticate", "Bearer");
    target.set_content(source.body.dump(), "application/json; charset=utf-8");
}
}
