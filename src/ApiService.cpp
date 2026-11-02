#include "ApiService.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <utility>
namespace AcApiV1 {
namespace {
bool ConstantTimeEqual(std::string const& left, std::string const& right) {
    std::size_t const size = std::max(left.size(), right.size());
    unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
    for (std::size_t index = 0; index < size; ++index) {
        unsigned char const lhs = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        unsigned char const rhs = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= static_cast<unsigned char>(lhs ^ rhs);
    }
    return difference == 0;
}
std::string Header(Request const& request, std::string const& name) {
    auto const found = request.headers.find(name);
    return found == request.headers.end() ? std::string() : found->second;
}
bool ParseUnsigned(std::string const& text, std::uint32_t& value) {
    if (text.empty()) return false;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
bool QuerySize(Request const& request, std::string const& name, std::size_t fallback, std::size_t maximum, std::size_t& value) {
    auto const found = request.query.find(name);
    if (found == request.query.end()) { value = fallback; return true; }
    if (found->second.empty()) return false;
    unsigned long long parsed = 0;
    auto const result = std::from_chars(found->second.data(), found->second.data() + found->second.size(), parsed);
    if (result.ec != std::errc() || result.ptr != found->second.data() + found->second.size()) return false;
    value = static_cast<std::size_t>(std::min<unsigned long long>(parsed, maximum));
    return true;
}
std::string NormalizeUsername(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}
bool QueryAudience(Request const& request, PlayerAudience& audience) {
    auto const found = request.query.find("audience");
    std::string const value = found == request.query.end() ? "humans" : found->second;
    if (value == "humans") audience = PlayerAudience::Humans;
    else if (value == "ai") audience = PlayerAudience::Ai;
    else if (value == "all") audience = PlayerAudience::All;
    else return false;
    return true;
}
}
Response Error(int status, std::string const& code, std::string const& message) {
    return {status, {{"error", {{"code", code}, {"message", message}}}}};
}
ApiService::ApiService(ServiceConfig config, Backend& backend) : _config(std::move(config)), _backend(backend) {}
Response ApiService::Handle(Request const& request) {
    if (request.path == "/healthz" && request.method == "GET") return {200, {{"status", "ok"}}};
    if (request.path == "/readyz" && request.method == "GET")
        return _backend.IsReady() ? Response{200, {{"status", "ready"}}} : Error(503, "backend_unavailable", "The AzerothCore backend is not ready.");
    if (request.body.size() > _config.maxBodyBytes) return Error(413, "request_too_large", "The request body exceeds the configured limit.");
    return HandleAuthorized(request);
}
Response ApiService::HandleAuthorized(Request const& request) {
    if (request.path == "/v1/capabilities" && request.method == "GET") {
        if (!IsAuthorized(request, Scope::Either)) return Error(401, "unauthorized", "A valid bearer credential is required.");
        return {200, _backend.Capabilities()};
    }
    std::string const inspectPrefix = "/v1/accounts/by-username/";
    if (request.method == "GET" && request.path.rfind(inspectPrefix, 0) == 0) {
        if (!IsAuthorized(request, Scope::AccountWrite)) return Error(401, "unauthorized", "An account-write bearer credential is required.");
        std::string const username = NormalizeUsername(request.path.substr(inspectPrefix.size()));
        if (!IsValidUsername(username)) return Error(400, "invalid_username", "Username must contain 1-17 ASCII letters or digits.");
        return _backend.InspectAccount(username);
    }
    if (request.path == "/v1/accounts" && request.method == "POST") {
        if (!IsAuthorized(request, Scope::AccountWrite)) return Error(401, "unauthorized", "An account-write bearer credential is required.");
        std::string const idempotencyKey = Header(request, "idempotency-key");
        if (!IsValidIdempotencyKey(idempotencyKey)) return Error(400, "invalid_idempotency_key", "Idempotency-Key must contain 16-128 visible ASCII characters.");
        Json body;
        try { body = Json::parse(request.body); }
        catch (Json::exception const&) { return Error(400, "invalid_json", "The request body is not valid JSON."); }
        if (!body.is_object()) return Error(400, "invalid_request", "The request body must be a JSON object.");
        if (!body.contains("username") || !body["username"].is_string()
            || (body.contains("password") && !body["password"].is_string()))
            return Error(400, "invalid_request", "Username and password must be JSON strings.");
        std::string const username = NormalizeUsername(body["username"].get<std::string>());
        std::string const password = body.value("password", std::string());
        if (!IsValidUsername(username)) return Error(400, "invalid_username", "Username must contain 1-17 ASCII letters or digits.");
        if (!password.empty() && !IsValidPassword(password)) return Error(400, "invalid_password", "Password must contain 8-16 printable ASCII characters.");
        return _backend.CreateAccount(username, password, idempotencyKey);
    }
    std::string const accountPrefix = "/v1/accounts/";
    if (request.method == "POST" && request.path.rfind(accountPrefix, 0) == 0) {
        if (!IsAuthorized(request, Scope::AccountWrite)) return Error(401, "unauthorized", "An account-write bearer credential is required.");
        std::string const remainder = request.path.substr(accountPrefix.size());
        std::size_t const separator = remainder.find('/');
        if (separator == std::string::npos) return Error(404, "not_found", "No API route matches the request.");
        std::uint32_t accountId = 0;
        if (!ParseUnsigned(remainder.substr(0, separator), accountId) || accountId == 0) return Error(400, "invalid_account_id", "Account ID must be a positive integer.");
        return ParseAccountWrite(request, remainder.substr(separator + 1), accountId);
    }
    std::string const characterPrefix = "/v1/characters/";
    if (request.method == "GET" && request.path.rfind(characterPrefix, 0) == 0) {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        std::uint32_t characterId = 0;
        if (!ParseUnsigned(request.path.substr(characterPrefix.size()), characterId) || characterId == 0) return Error(400, "invalid_character_id", "Character ID must be a positive integer.");
        return _backend.CharacterStats(characterId);
    }
    std::string const guildPrefix = "/v1/guilds/";
    if (request.method == "GET" && request.path.rfind(guildPrefix, 0) == 0) {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        std::uint32_t guildId = 0;
        if (!ParseUnsigned(request.path.substr(guildPrefix.size()), guildId) || guildId == 0) return Error(400, "invalid_guild_id", "Guild ID must be a positive integer.");
        std::size_t limit = 0;
        std::size_t offset = 0;
        if (!QuerySize(request, "limit", 25, _config.maxPageSize, limit)
            || !QuerySize(request, "offset", 0, std::numeric_limits<std::uint32_t>::max(), offset))
            return Error(400, "invalid_pagination", "Limit and offset must be unsigned integers.");
        if (limit == 0) return Error(400, "invalid_page_size", "Limit must be greater than zero.");
        return _backend.GuildStats(guildId, limit, offset);
    }
    if (request.path == "/v1/realm/status" && request.method == "GET") {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        return _backend.RealmStatus();
    }
    if (request.path == "/v1/players/online" && request.method == "GET") {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        PlayerAudience audience = PlayerAudience::Humans;
        if (!QueryAudience(request, audience)) return Error(400, "invalid_audience", "Audience must be humans, ai, or all.");
        std::size_t limit = 0;
        std::size_t offset = 0;
        if (!QuerySize(request, "limit", 25, _config.maxPageSize, limit)
            || !QuerySize(request, "offset", 0, std::numeric_limits<std::uint32_t>::max(), offset))
            return Error(400, "invalid_pagination", "Limit and offset must be unsigned integers.");
        if (limit == 0) return Error(400, "invalid_page_size", "Limit must be greater than zero.");
        return _backend.OnlinePlayers(audience, limit, offset);
    }
    if (request.path == "/v1/leaderboards/players" && request.method == "GET") {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        std::string metric = "level";
        if (auto const found = request.query.find("metric"); found != request.query.end()) metric = found->second;
        if (metric != "level" && metric != "playtime" && metric != "honor" && metric != "kills") return Error(400, "invalid_metric", "Metric must be level, playtime, honor, or kills.");
        PlayerAudience audience = PlayerAudience::Humans;
        if (!QueryAudience(request, audience)) return Error(400, "invalid_audience", "Audience must be humans, ai, or all.");
        std::size_t limit = 0;
        std::size_t offset = 0;
        if (!QuerySize(request, "limit", 25, _config.maxPageSize, limit)
            || !QuerySize(request, "offset", 0, std::numeric_limits<std::uint32_t>::max(), offset))
            return Error(400, "invalid_pagination", "Limit and offset must be unsigned integers.");
        if (limit == 0) return Error(400, "invalid_page_size", "Limit must be greater than zero.");
        return _backend.PlayerLeaderboard(metric, audience, limit, offset);
    }
    return Error(404, "not_found", "No API route matches the request.");
}
Response ApiService::ParseAccountWrite(Request const& request, std::string const& operation, std::uint32_t accountId) {
    std::string const idempotencyKey = Header(request, "idempotency-key");
    if (!IsValidIdempotencyKey(idempotencyKey)) return Error(400, "invalid_idempotency_key", "Idempotency-Key must contain 16-128 visible ASCII characters.");
    if (operation == "password-reset") return _backend.ResetPassword(accountId, idempotencyKey);
    if (operation == "suspend") return _backend.SuspendAccount(accountId, idempotencyKey);
    if (operation == "restore") return _backend.RestoreAccount(accountId, idempotencyKey);
    return Error(404, "not_found", "No API route matches the request.");
}
bool ApiService::IsAuthorized(Request const& request, Scope scope) const {
    std::string const authorization = Header(request, "authorization");
    std::string const prefix = "Bearer ";
    if (authorization.rfind(prefix, 0) != 0) return false;
    std::string const supplied = authorization.substr(prefix.size());
    bool const account = !_config.accountToken.empty() && ConstantTimeEqual(supplied, _config.accountToken);
    bool const community = !_config.communityToken.empty() && ConstantTimeEqual(supplied, _config.communityToken);
    if (scope == Scope::AccountWrite) return account;
    if (scope == Scope::CommunityRead) return community;
    return account || community;
}
bool ApiService::IsValidUsername(std::string const& value) {
    return !value.empty() && value.size() <= 17 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'); });
}
bool ApiService::IsValidPassword(std::string const& value) {
    return value.size() >= 8 && value.size() <= 16 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return character >= 0x21 && character <= 0x7e; });
}
bool ApiService::IsValidIdempotencyKey(std::string const& value) {
    return value.size() >= 16 && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return character >= 0x21 && character <= 0x7e; });
}
}
