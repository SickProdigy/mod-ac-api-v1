#include "ApiService.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace AcApiV1;
namespace {
class FakeBackend final : public Backend {
public:
    bool ready = true; std::string lastOperation; std::string lastUsername; std::string lastPassword; std::string lastKey; std::uint32_t lastAccountId = 0;
    bool IsReady() const override { return ready; }
    Json Capabilities() const override { return {{"api_version", "v1"}}; }
    Response InspectAccount(std::string const& username) override { lastOperation = "inspect"; lastUsername = username; return {200, {{"username", username}}}; }
    Response CreateAccount(std::string const& username, std::string const& password, std::string const& key) override { lastOperation = "create"; lastUsername = username; lastPassword = password; lastKey = key; return {201, {{"id", 42}}}; }
    Response ResetPassword(std::uint32_t id, std::string const& key) override { lastOperation = "reset"; lastAccountId = id; lastKey = key; return {200, {{"id", id}}}; }
    Response SuspendAccount(std::uint32_t id, std::string const& key) override { lastOperation = "suspend"; lastAccountId = id; lastKey = key; return {200, {{"id", id}}}; }
    Response RestoreAccount(std::uint32_t id, std::string const& key) override { lastOperation = "restore"; lastAccountId = id; lastKey = key; return {200, {{"id", id}}}; }
    Response PlayerLeaderboard(std::string const& metric, std::size_t limit, std::size_t offset) override { lastOperation = "leaderboard"; return {200, {{"metric", metric}, {"limit", limit}, {"offset", offset}}}; }
};
int failures = 0;
void Expect(bool condition, std::string const& message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }
Request Authorized(std::string method, std::string path, std::string token) { Request request; request.method = std::move(method); request.path = std::move(path); request.headers["authorization"] = "Bearer " + token; return request; }
}
int main() {
    FakeBackend backend;
    ServiceConfig config;
    config.accountToken = "account-secret";
    config.communityToken = "community-secret";
    config.maxBodyBytes = 128;
    config.maxPageSize = 50;
    ApiService service(config, backend);
    Request health; health.method = "GET"; health.path = "/healthz";
    Expect(service.Handle(health).status == 200, "health is public");
    Request readiness; readiness.method = "GET"; readiness.path = "/readyz"; backend.ready = false;
    Expect(service.Handle(readiness).status == 503, "readiness reflects backend"); backend.ready = true;
    Request inspect = Authorized("GET", "/v1/accounts/by-username/testuser", "account-secret");
    Response response = service.Handle(inspect);
    Expect(response.status == 200 && backend.lastUsername == "TESTUSER", "inspect normalizes valid usernames");
    Request denied = Authorized("GET", "/v1/accounts/by-username/testuser", "community-secret");
    Expect(service.Handle(denied).status == 401, "community token cannot inspect accounts");
    Request create = Authorized("POST", "/v1/accounts", "account-secret");
    create.headers["idempotency-key"] = "1234567890abcdef";
    create.body = R"({"username":"newuser","password":"validPass9!"})";
    response = service.Handle(create);
    Expect(response.status == 201 && backend.lastOperation == "create" && backend.lastUsername == "NEWUSER" && backend.lastPassword == "validPass9!", "valid account creation reaches backend");
    Request missingKey = create; missingKey.headers.erase("idempotency-key");
    Expect(service.Handle(missingKey).status == 400, "writes require idempotency keys");
    Request invalidJson = create; invalidJson.body = "{";
    Expect(service.Handle(invalidJson).status == 400, "malformed JSON is rejected");
    Request reset = Authorized("POST", "/v1/accounts/42/password-reset", "account-secret");
    reset.headers["idempotency-key"] = "abcdefghijklmnop"; response = service.Handle(reset);
    Expect(response.status == 200 && backend.lastOperation == "reset" && backend.lastAccountId == 42, "password reset route is parsed");
    Request leaderboard = Authorized("GET", "/v1/leaderboards/players", "community-secret");
    leaderboard.query = {{"metric", "playtime"}, {"limit", "500"}, {"offset", "10"}}; response = service.Handle(leaderboard);
    Expect(response.status == 200 && response.body["limit"] == 50 && response.body["offset"] == 10, "leaderboard pagination is bounded");
    Request wrongScope = Authorized("GET", "/v1/leaderboards/players", "account-secret");
    Expect(service.Handle(wrongScope).status == 401, "account token cannot read community data");
    Request oversized = create; oversized.body = std::string(129, 'x');
    Expect(service.Handle(oversized).status == 413, "request bodies are bounded");
    Expect(ApiService::IsValidUsername("ABC123"), "valid username accepted");
    Expect(!ApiService::IsValidUsername("ABC_123"), "username punctuation rejected");
    Expect(!ApiService::IsValidPassword("short"), "short password rejected");
    Expect(ApiService::IsValidPassword("longEnough9!"), "valid password accepted");
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "All ApiService tests passed\n";
    return EXIT_SUCCESS;
}
