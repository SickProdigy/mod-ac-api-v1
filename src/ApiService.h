#ifndef AC_API_V1_API_SERVICE_H
#define AC_API_V1_API_SERVICE_H
#include "../deps/nlohmann/json.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
namespace AcApiV1 {
using Json = nlohmann::json;
struct Request { std::string method; std::string path; std::map<std::string, std::string> headers; std::map<std::string, std::string> query; std::string body; };
struct Response { int status = 500; Json body; };
struct ServiceConfig { std::string accountToken; std::string communityToken; std::size_t maxBodyBytes = 16384; std::size_t maxPageSize = 100; };
enum class PlayerAudience { Humans, Ai, All };
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool IsReady() const = 0;
    virtual Json Capabilities() const = 0;
    virtual Response InspectAccount(std::string const& username) = 0;
    virtual Response CreateAccount(std::string const& username, std::string const& password, std::string const& idempotencyKey) = 0;
    virtual Response ResetPassword(std::uint32_t accountId, std::string const& idempotencyKey) = 0;
    virtual Response SuspendAccount(std::uint32_t accountId, std::string const& idempotencyKey) = 0;
    virtual Response RestoreAccount(std::uint32_t accountId, std::string const& idempotencyKey) = 0;
    virtual Response CharacterStats(std::uint32_t characterId) = 0;
    virtual Response GuildStats(std::uint32_t guildId, std::size_t limit, std::size_t offset) = 0;
    virtual Response RealmStatus(PlayerAudience audience) = 0;
    virtual Response OnlinePlayers(PlayerAudience audience, std::size_t limit, std::size_t offset) = 0;
    virtual Response PlayerLeaderboard(std::string const& metric, PlayerAudience audience,
        std::size_t limit, std::size_t offset) = 0;
};
class ApiService {
public:
    ApiService(ServiceConfig config, Backend& backend);
    Response Handle(Request const& request);
    static bool IsValidUsername(std::string const& value);
    static bool IsValidPassword(std::string const& value);
    static bool IsValidIdempotencyKey(std::string const& value);
private:
    enum class Scope { AccountWrite, CommunityRead, Either };
    Response HandleAuthorized(Request const& request);
    Response ParseAccountWrite(Request const& request, std::string const& operation, std::uint32_t accountId);
    bool IsAuthorized(Request const& request, Scope scope) const;
    ServiceConfig _config;
    Backend& _backend;
};
Response Error(int status, std::string const& code, std::string const& message);
}
#endif
