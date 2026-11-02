#include "ApiService.h"
#include "AzerothCoreBackend.h"
#include "HttpServer.h"

#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <memory>
#include <string>

namespace AcApiV1
{
namespace
{
std::unique_ptr<AzerothCoreBackend> BackendInstance;
std::unique_ptr<ApiService> ServiceInstance;
std::unique_ptr<HttpServer> ServerInstance;

bool IsPrivateBind(std::string const& address)
{
    if (address == "localhost")
        return true;

    in_addr ipv4{};
    if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1)
    {
        std::uint32_t const host = ntohl(ipv4.s_addr);
        std::uint8_t const first = static_cast<std::uint8_t>(host >> 24);
        std::uint8_t const second = static_cast<std::uint8_t>(host >> 16);
        return first == 127 || first == 10 || (first == 172 && second >= 16 && second <= 31)
            || (first == 192 && second == 168)
            || (first == 100 && second >= 64 && second <= 127);
    }

    in6_addr ipv6{};
    if (inet_pton(AF_INET6, address.c_str(), &ipv6) == 1)
        return IN6_IS_ADDR_LOOPBACK(&ipv6) || (ipv6.s6_addr[0] & 0xfe) == 0xfc;

    return false;
}

bool IsDatabaseIdentifier(std::string const& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character)
    {
        return std::isalnum(character) || character == '_';
    });
}

void StopServer()
{
    if (ServerInstance)
        ServerInstance->Stop();
    ServerInstance.reset();
    ServiceInstance.reset();
    BackendInstance.reset();
}

bool StartServer()
{
    if (!sConfigMgr->GetOption<bool>("AcApiV1.Enable", false))
        return true;

    ServiceConfig serviceConfig;
    serviceConfig.accountToken =
        sConfigMgr->GetOption<std::string>("AcApiV1.AccountWriteToken", "");
    serviceConfig.communityToken =
        sConfigMgr->GetOption<std::string>("AcApiV1.CommunityReadToken", "");
    serviceConfig.maxBodyBytes =
        sConfigMgr->GetOption<std::uint32_t>("AcApiV1.MaxRequestBodyBytes", 16384);
    serviceConfig.maxPageSize =
        sConfigMgr->GetOption<std::uint32_t>("AcApiV1.MaxPageSize", 100);

    std::string const derivationSecret =
        sConfigMgr->GetOption<std::string>("AcApiV1.CredentialDerivationSecret", "");
    std::string const communityDatabaseInfo =
        sConfigMgr->GetOption<std::string>("AcApiV1.CommunityDatabaseInfo", "");
    std::string const playerbotsDatabaseName =
        sConfigMgr->GetOption<std::string>("AcApiV1.PlayerbotsDatabaseName", "acore_playerbots");
    if (serviceConfig.accountToken.size() < 32 || serviceConfig.communityToken.size() < 32
        || derivationSecret.size() < 32 || communityDatabaseInfo.empty()
        || !IsDatabaseIdentifier(playerbotsDatabaseName))
    {
        LOG_ERROR("module", "ac-api-v1 refused to start: all API credentials must be at least 32 characters, CommunityDatabaseInfo must be set, and PlayerbotsDatabaseName must be a valid database identifier");
        return false;
    }
    if (serviceConfig.accountToken == serviceConfig.communityToken)
    {
        LOG_ERROR("module", "ac-api-v1 refused to start: account-write and community-read credentials must differ");
        return false;
    }

    HttpServerConfig httpConfig;
    httpConfig.bindAddress =
        sConfigMgr->GetOption<std::string>("AcApiV1.BindAddress", "127.0.0.1");
    httpConfig.port = sConfigMgr->GetOption<int32>("AcApiV1.Port", 8090);
    httpConfig.workerThreads = std::clamp(
        sConfigMgr->GetOption<int32>("AcApiV1.WorkerThreads", 4), 1, 32);
    httpConfig.maxQueuedRequests = std::clamp(
        sConfigMgr->GetOption<int32>("AcApiV1.MaxQueuedRequests", 64), 1, 1024);
    httpConfig.timeoutSeconds = std::clamp(
        sConfigMgr->GetOption<int32>("AcApiV1.TimeoutSeconds", 5), 1, 30);
    httpConfig.requestsPerMinute = std::clamp(
        sConfigMgr->GetOption<int32>("AcApiV1.RequestsPerMinute", 120), 1, 10000);
    httpConfig.maxBodyBytes = serviceConfig.maxBodyBytes;

    if (!IsPrivateBind(httpConfig.bindAddress))
    {
        LOG_ERROR("module", "ac-api-v1 refused to bind to non-private address '{}'",
            httpConfig.bindAddress);
        return false;
    }
    if (httpConfig.port < 1 || httpConfig.port > 65535)
    {
        LOG_ERROR("module", "ac-api-v1 refused invalid port {}", httpConfig.port);
        return false;
    }

    std::uint8_t const communityDatabaseThreads = static_cast<std::uint8_t>(std::clamp(
        sConfigMgr->GetOption<int32>("AcApiV1.CommunityDatabaseThreads", 2), 1, 8));
    BackendInstance = std::make_unique<AzerothCoreBackend>(
        derivationSecret, communityDatabaseInfo, playerbotsDatabaseName,
        communityDatabaseThreads);
    ServiceInstance = std::make_unique<ApiService>(serviceConfig, *BackendInstance);
    ServerInstance = std::make_unique<HttpServer>(httpConfig, *ServiceInstance);
    if (!ServerInstance->Start())
    {
        LOG_ERROR("module", "ac-api-v1 could not listen on {}:{}", httpConfig.bindAddress,
            httpConfig.port);
        StopServer();
        return false;
    }

    LOG_INFO("module", "ac-api-v1 listening on private address {}:{}", httpConfig.bindAddress,
        httpConfig.port);
    return true;
}

class AcApiV1WorldScript final : public WorldScript
{
public:
    AcApiV1WorldScript()
        : WorldScript("AcApiV1WorldScript", {
              WORLDHOOK_ON_AFTER_CONFIG_LOAD,
              WORLDHOOK_ON_STARTUP,
              WORLDHOOK_ON_UPDATE,
              WORLDHOOK_ON_SHUTDOWN
          })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload && ServerInstance)
        {
            StopServer();
            StartServer();
        }
    }

    void OnStartup() override
    {
        StartServer();
    }

    void OnUpdate(uint32 diff) override
    {
        _keepAliveElapsed += diff;
        if (_keepAliveElapsed >= 60000)
        {
            _keepAliveElapsed = 0;
            if (BackendInstance)
                BackendInstance->KeepAlive();
        }
    }

    void OnShutdown() override
    {
        StopServer();
    }

private:
    uint32 _keepAliveElapsed = 0;
};
}
}

void AddAcApiV1Scripts()
{
    new AcApiV1::AcApiV1WorldScript();
}
