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
std::size_t QuerySize(Request const& request, std::string const& name, std::size_t fallback, std::size_t maximum) {
    auto const found = request.query.find(name);
    if (found == request.query.end() || found->second.empty()) return fallback;
    unsigned long long parsed = 0;
    auto const result = std::from_chars(found->second.data(), found->second.data() + found->second.size(), parsed);
    if (result.ec != std::errc() || result.ptr != found->second.data() + found->second.size()) return fallback;
    return static_cast<std::size_t>(std::min<unsigned long long>(parsed, maximum));
}
std::string NormalizeUsername(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
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
        std::string const username = NormalizeUsername(body.value("username", ""));
        std::string const password = body.value("password", "");
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
    if (request.path == "/v1/leaderboards/players" && request.method == "GET") {
        if (!IsAuthorized(request, Scope::CommunityRead)) return Error(401, "unauthorized", "A community-read bearer credential is required.");
        std::string metric = "level";
        if (auto const found = request.query.find("metric"); found != request.query.end()) metric = found->second;
        if (metric != "level" && metric != "playtime" && metric != "honor" && metric != "kills") return Error(400, "invalid_metric", "Metric must be level, playtime, honor, or kills.");
        std::size_t const limit = QuerySize(request, "limit", 25, _config.maxPageSize);
        std::size_t const offset = QuerySize(request, "offset", 0, std::numeric_limits<std::uint32_t>::max());
        if (limit == 0) return Error(400, "invalid_page_size", "Limit must be greater than zero.");
        return _backend.PlayerLeaderboard(metric, limit, offset);
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
    return !value.empty() && value.size() <= 17 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isalnum(character) != 0; });
}
bool ApiService::IsValidPassword(std::string const& value) {
    return value.size() >= 8 && value.size() <= 16 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return character >= 0x21 && character <= 0x7e; });
}
bool ApiService::IsValidIdempotencyKey(std::string const& value) {
    return value.size() >= 16 && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return character >= 0x21 && character <= 0x7e; });
}
}
