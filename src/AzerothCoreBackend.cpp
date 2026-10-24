#include "AzerothCoreBackend.h"

#include "AccountMgr.h"
#include "BanMgr.h"
#include "CryptoHash.h"
#include "DatabaseEnv.h"
#include "GitRevision.h"
#include "HMAC.h"
#include "ModuleDatabasePool.h"
#include "MySQLConnection.h"
#include "QueryResult.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace AcApiV1
{
class CommunityDatabaseConnection final : public MySQLConnection
{
public:
    explicit CommunityDatabaseConnection(MySQLConnectionInfo& info) : MySQLConnection(info) {}

private:
    void DoPrepareStatements() override {}
};

class CommunityDatabasePool final : public ModuleDatabasePool
{
protected:
    MySQLConnection* CreateConnection(MySQLConnectionInfo& info) override
    {
        return new CommunityDatabaseConnection(info);
    }
};

namespace
{
constexpr char SuspensionAuthor[] = "ac-api-v1";
constexpr char SuspensionReason[] = "ac-api-v1 managed suspension";

std::string Hex(auto const& bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (auto const byte : bytes)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}

std::string Escaped(std::string value)
{
    LoginDatabase.EscapeString(value);
    return value;
}

Response OperationConflict()
{
    return Error(409, "idempotency_conflict",
        "The idempotency key was already used for a different request.");
}

std::string AccountResultCode(AccountOpResult result)
{
    switch (result)
    {
        case AOR_OK:
            return "ok";
        case AOR_NAME_TOO_LONG:
            return "invalid_username";
        case AOR_PASS_TOO_LONG:
            return "invalid_password";
        case AOR_NAME_ALREADY_EXIST:
            return "account_collision";
        case AOR_NAME_NOT_EXIST:
            return "account_not_found";
        case AOR_EMAIL_TOO_LONG:
            return "invalid_email";
        case AOR_DB_INTERNAL_ERROR:
        default:
            return "backend_error";
    }
}
}

AzerothCoreBackend::AzerothCoreBackend(std::string credentialDerivationSecret,
    std::string communityDatabaseInfo, std::uint8_t communityDatabaseThreads)
    : _credentialDerivationSecret(std::move(credentialDerivationSecret)),
      _communityDatabase(std::make_unique<CommunityDatabasePool>())
{
    if (!communityDatabaseInfo.empty())
    {
        _communityDatabase->SetConnectionInfo(communityDatabaseInfo, communityDatabaseThreads);
        _communityDatabaseReady = _communityDatabase->Open() == 0
            && _communityDatabase->PrepareStatements();
    }
}

AzerothCoreBackend::~AzerothCoreBackend() = default;

void AzerothCoreBackend::KeepAlive()
{
    if (_communityDatabaseReady)
        _communityDatabase->KeepAlive();
}

bool AzerothCoreBackend::IsReady() const
{
    return _communityDatabaseReady
        && static_cast<bool>(LoginDatabase.Query("SELECT COUNT(*) FROM ac_api_v1_idempotency"))
        && static_cast<bool>(_communityDatabase->Query("SELECT 1"));
}

Json AzerothCoreBackend::Capabilities() const
{
    return {
        {"api_version", "v1"},
        {"azerothcore", {
            {"revision", GitRevision::GetHash()},
            {"version", GitRevision::GetFullVersion()}
        }},
        {"account_operations", {
            {"inspect", true},
            {"create", true},
            {"password_reset", true},
            {"native_suspend", true},
            {"native_restore", true},
            {"managed_accounts_only", true}
        }},
        {"community_data", {
            {"player_leaderboard", true},
            {"character_stats", true},
            {"guild_stats", true},
            {"realm_status", true},
            {"separate_read_only_database", true},
            {"metrics", {"level", "playtime", "honor", "kills"}}
        }}
    };
}

Response AzerothCoreBackend::InspectAccount(std::string const& username)
{
    std::uint32_t const accountId = AccountMgr::GetId(username);
    if (accountId == 0)
        return Error(404, "account_not_found", "No account exists with that username.");

    bool const managed = IsManaged(accountId);
    BanState const ban = GetBanState(accountId);
    std::string state = "active";
    if (!managed)
        state = "unmanaged";
    else if (ban.active && ban.managed)
        state = "suspended";
    else if (ban.active)
        state = "restricted";

    return {200, {
        {"id", accountId},
        {"username", username},
        {"managed", managed},
        {"state", state},
        {"player_account", AccountMgr::IsPlayerAccount(AccountMgr::GetSecurity(accountId))}
    }};
}

Response AzerothCoreBackend::CreateAccount(std::string const& username, std::string const& suppliedPassword,
    std::string const& idempotencyKey)
{
    bool const generated = suppliedPassword.empty();
    std::string const keyHash = Hash(idempotencyKey);
    std::string const requestFingerprint = Hash(username + "\n"
        + (generated ? "generated" : Hash(suppliedPassword)));
    IdempotencyRecord const record = Claim(idempotencyKey, "account.create", requestFingerprint);
    if (!record.exists)
        return Error(503, "backend_unavailable", "The idempotency store is unavailable.");
    if (record.operation != "account.create" || record.requestHash != requestFingerprint)
        return OperationConflict();

    std::string password = suppliedPassword;
    if (generated)
        password = DerivePassword("account.create", 0, idempotencyKey);

    if (record.state == "completed")
        return ExistingResult(record, generated ? password : "");

    std::uint32_t accountId = AccountMgr::GetId(username);
    if (accountId != 0)
    {
        if (!IsManagedUsername(username, keyHash))
        {
            Response const response = Complete(idempotencyKey, "account.create", 409,
                Error(409, "account_collision",
                    "The username already belongs to an unmanaged or different account.").body);
            Audit("account.create", accountId, "account_collision");
            return response;
        }
    }
    else
    {
        if (!ReserveUsername(username, keyHash))
        {
            Response const response = Complete(idempotencyKey, "account.create", 409,
                Error(409, "account_collision",
                    "The username is reserved by another account operation.").body);
            Audit("account.create", 0, "account_collision");
            return response;
        }

        AccountOpResult const result = sAccountMgr->CreateAccount(username, password);
        if (result != AOR_OK && result != AOR_NAME_ALREADY_EXIST)
        {
            std::string const code = AccountResultCode(result);
            int const status = result == AOR_DB_INTERNAL_ERROR ? 503 : 400;
            Audit("account.create", 0, code);
            return Error(status, code, "AzerothCore rejected the account creation request.");
        }

        for (int attempt = 0; attempt < 20 && accountId == 0; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            accountId = AccountMgr::GetId(username);
        }

        if (accountId == 0)
        {
            Audit("account.create", 0, "partial_success");
            return Error(503, "partial_success",
                "Account creation was accepted but is not yet visible; retry with the same idempotency key.");
        }
    }

    if (!AccountMgr::IsPlayerAccount(AccountMgr::GetSecurity(accountId)))
    {
        Audit("account.create", accountId, "privileged_account");
        return Error(409, "privileged_account",
            "The resolved account is privileged and cannot be managed by this API.");
    }

    if (!RegisterManaged(accountId, username, keyHash))
    {
        Audit("account.create", accountId, "mapping_conflict");
        return Error(409, "mapping_conflict",
            "The account exists but its managed mapping conflicts with another operation.");
    }

    Json stored = {
        {"id", accountId},
        {"username", username},
        {"managed", true},
        {"state", "active"},
        {"credential_generated", generated}
    };
    Response response = Complete(idempotencyKey, "account.create", 201, stored, accountId);
    if (generated)
        response.body["password"] = password;
    Audit("account.create", accountId, "created");
    return response;
}

Response AzerothCoreBackend::ResetPassword(std::uint32_t accountId, std::string const& idempotencyKey)
{
    std::string const fingerprint = Hash(std::to_string(accountId));
    IdempotencyRecord const record = Claim(idempotencyKey, "account.password_reset", fingerprint);
    if (!record.exists)
        return Error(503, "backend_unavailable", "The idempotency store is unavailable.");
    if (record.operation != "account.password_reset" || record.requestHash != fingerprint)
        return OperationConflict();

    std::string const password = DerivePassword("account.password_reset", accountId, idempotencyKey);
    if (record.state == "completed")
        return ExistingResult(record, password);

    std::string username;
    Response const validation = ValidateManagedAccount(accountId, username);
    if (validation.status != 200)
        return validation;

    AccountOpResult const result = AccountMgr::ChangePassword(accountId, password);
    if (result != AOR_OK)
    {
        std::string const code = AccountResultCode(result);
        Audit("account.password_reset", accountId, code);
        return Error(result == AOR_DB_INTERNAL_ERROR ? 503 : 400, code,
            "AzerothCore rejected the password reset.");
    }

    Json stored = {{"id", accountId}, {"username", username}, {"credential_generated", true}};
    Response response = Complete(idempotencyKey, "account.password_reset", 200, stored, accountId);
    response.body["password"] = password;
    Audit("account.password_reset", accountId, "completed");
    return response;
}

Response AzerothCoreBackend::SuspendAccount(std::uint32_t accountId, std::string const& idempotencyKey)
{
    std::string const fingerprint = Hash(std::to_string(accountId));
    IdempotencyRecord const record = Claim(idempotencyKey, "account.suspend", fingerprint);
    if (!record.exists)
        return Error(503, "backend_unavailable", "The idempotency store is unavailable.");
    if (record.operation != "account.suspend" || record.requestHash != fingerprint)
        return OperationConflict();
    if (record.state == "completed")
        return ExistingResult(record);

    std::string username;
    Response const validation = ValidateManagedAccount(accountId, username);
    if (validation.status != 200)
        return validation;

    BanState const ban = GetBanState(accountId);
    if (ban.active && !ban.managed)
    {
        Audit("account.suspend", accountId, "suspension_conflict");
        return Error(409, "suspension_conflict",
            "The account has a non-API ban that this service will not replace.");
    }

    if (!ban.active)
    {
        BanReturn const result = sBan->BanAccount(username, "0", SuspensionReason, SuspensionAuthor);
        if (result != BAN_SUCCESS)
        {
            Audit("account.suspend", accountId, "backend_error");
            return Error(503, "backend_error", "AzerothCore could not suspend the account.");
        }
    }

    LoginDatabase.DirectExecute(
        "INSERT INTO ac_api_v1_managed_suspension (account_id, active) VALUES ({}, 1) "
        "ON DUPLICATE KEY UPDATE active = 1", accountId);
    Json const body = {{"id", accountId}, {"username", username}, {"state", "suspended"}};
    Response const response = Complete(idempotencyKey, "account.suspend", 200, body, accountId);
    Audit("account.suspend", accountId, "completed");
    return response;
}

Response AzerothCoreBackend::RestoreAccount(std::uint32_t accountId, std::string const& idempotencyKey)
{
    std::string const fingerprint = Hash(std::to_string(accountId));
    IdempotencyRecord const record = Claim(idempotencyKey, "account.restore", fingerprint);
    if (!record.exists)
        return Error(503, "backend_unavailable", "The idempotency store is unavailable.");
    if (record.operation != "account.restore" || record.requestHash != fingerprint)
        return OperationConflict();
    if (record.state == "completed")
        return ExistingResult(record);

    std::string username;
    Response const validation = ValidateManagedAccount(accountId, username);
    if (validation.status != 200)
        return validation;
    if (!HasManagedSuspension(accountId))
        return Error(409, "not_api_suspended",
            "The account does not have an active API-managed suspension.");

    BanState const ban = GetBanState(accountId);
    if (ban.active && !ban.managed)
    {
        Audit("account.restore", accountId, "suspension_conflict");
        return Error(409, "suspension_conflict",
            "The active ban is not API-managed and will not be removed.");
    }

    if (ban.active && !sBan->RemoveBanAccount(username))
    {
        Audit("account.restore", accountId, "backend_error");
        return Error(503, "backend_error", "AzerothCore could not restore the account.");
    }

    LoginDatabase.DirectExecute(
        "UPDATE ac_api_v1_managed_suspension SET active = 0 WHERE account_id = {}", accountId);
    Json const body = {{"id", accountId}, {"username", username}, {"state", "active"}};
    Response const response = Complete(idempotencyKey, "account.restore", 200, body, accountId);
    Audit("account.restore", accountId, "completed");
    return response;
}

Response AzerothCoreBackend::CharacterStats(std::uint32_t characterId)
{
    if (!_communityDatabaseReady)
        return Error(503, "backend_unavailable", "The community database is unavailable.");

    QueryResult result = _communityDatabase->Query(
        "SELECT c.guid, c.name, c.race, c.class, c.gender, c.level, c.totaltime, "
        "c.totalHonorPoints, c.totalKills, c.money, c.online, "
        "COALESCE(g.guildid, 0), COALESCE(g.name, '') "
        "FROM characters c "
        "LEFT JOIN guild_member gm ON gm.guid = c.guid "
        "LEFT JOIN guild g ON g.guildid = gm.guildid "
        "WHERE c.guid = {} AND c.deleteDate IS NULL", characterId);
    if (!result)
        return Error(404, "character_not_found", "No active character exists with that ID.");

    Field* fields = result->Fetch();
    return {200, {
        {"character_id", fields[0].Get<std::uint32_t>()},
        {"name", fields[1].Get<std::string>()},
        {"race", fields[2].Get<std::uint8_t>()},
        {"class", fields[3].Get<std::uint8_t>()},
        {"gender", fields[4].Get<std::uint8_t>()},
        {"level", fields[5].Get<std::uint8_t>()},
        {"playtime_seconds", fields[6].Get<std::uint32_t>()},
        {"honor", fields[7].Get<std::uint32_t>()},
        {"kills", fields[8].Get<std::uint32_t>()},
        {"money_copper", fields[9].Get<std::uint32_t>()},
        {"online", fields[10].Get<std::uint8_t>() != 0},
        {"guild", {
            {"id", fields[11].Get<std::uint32_t>()},
            {"name", fields[12].Get<std::string>()}
        }}
    }};
}

Response AzerothCoreBackend::GuildStats(std::uint32_t guildId, std::size_t limit,
    std::size_t offset)
{
    if (!_communityDatabaseReady)
        return Error(503, "backend_unavailable", "The community database is unavailable.");

    QueryResult guild = _communityDatabase->Query(
        "SELECT g.guildid, g.name, g.leaderguid, COUNT(gm.guid) "
        "FROM guild g LEFT JOIN guild_member gm ON gm.guildid = g.guildid "
        "WHERE g.guildid = {} GROUP BY g.guildid, g.name, g.leaderguid", guildId);
    if (!guild)
        return Error(404, "guild_not_found", "No guild exists with that ID.");

    Field* guildFields = guild->Fetch();
    Json members = Json::array();
    QueryResult memberRows = _communityDatabase->Query(
        "SELECT c.guid, c.name, c.race, c.class, c.level, gm.rank "
        "FROM guild_member gm JOIN characters c ON c.guid = gm.guid "
        "WHERE gm.guildid = {} AND c.deleteDate IS NULL "
        "ORDER BY gm.rank ASC, c.name ASC LIMIT {} OFFSET {}", guildId, limit, offset);
    if (memberRows)
    {
        do
        {
            Field* fields = memberRows->Fetch();
            members.push_back({
                {"character_id", fields[0].Get<std::uint32_t>()},
                {"name", fields[1].Get<std::string>()},
                {"race", fields[2].Get<std::uint8_t>()},
                {"class", fields[3].Get<std::uint8_t>()},
                {"level", fields[4].Get<std::uint8_t>()},
                {"rank", fields[5].Get<std::uint8_t>()}
            });
        } while (memberRows->NextRow());
    }

    return {200, {
        {"guild_id", guildFields[0].Get<std::uint32_t>()},
        {"name", guildFields[1].Get<std::string>()},
        {"leader_character_id", guildFields[2].Get<std::uint32_t>()},
        {"member_count", guildFields[3].Get<std::uint64_t>()},
        {"limit", limit},
        {"offset", offset},
        {"members", std::move(members)}
    }};
}

Response AzerothCoreBackend::RealmStatus()
{
    if (!_communityDatabaseReady)
        return Error(503, "backend_unavailable", "The community database is unavailable.");

    QueryResult result = _communityDatabase->Query(
        "SELECT COUNT(*), COALESCE(SUM(online <> 0), 0) "
        "FROM characters WHERE deleteDate IS NULL");
    if (!result)
        return Error(503, "backend_unavailable", "The community database did not return realm status.");

    Field* fields = result->Fetch();
    return {200, {
        {"status", "online"},
        {"characters", fields[0].Get<std::uint64_t>()},
        {"online_characters", fields[1].Get<std::uint64_t>()}
    }};
}

Response AzerothCoreBackend::PlayerLeaderboard(std::string const& metric, std::size_t limit,
    std::size_t offset)
{
    if (!_communityDatabaseReady)
        return Error(503, "backend_unavailable", "The community database is unavailable.");

    std::string column;
    if (metric == "level")
        column = "level";
    else if (metric == "playtime")
        column = "totaltime";
    else if (metric == "honor")
        column = "totalHonorPoints";
    else if (metric == "kills")
        column = "totalKills";
    else
        return Error(400, "invalid_metric", "The requested leaderboard metric is not supported.");

    QueryResult result = _communityDatabase->Query(
        "SELECT guid, name, race, class, level, totaltime, totalHonorPoints, totalKills "
        "FROM characters WHERE deleteDate IS NULL ORDER BY {} DESC, guid ASC LIMIT {} OFFSET {}",
        column, limit, offset);

    Json entries = Json::array();
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            entries.push_back({
                {"character_id", fields[0].Get<std::uint32_t>()},
                {"name", fields[1].Get<std::string>()},
                {"race", fields[2].Get<std::uint8_t>()},
                {"class", fields[3].Get<std::uint8_t>()},
                {"level", fields[4].Get<std::uint8_t>()},
                {"playtime_seconds", fields[5].Get<std::uint32_t>()},
                {"honor", fields[6].Get<std::uint32_t>()},
                {"kills", fields[7].Get<std::uint32_t>()}
            });
        } while (result->NextRow());
    }

    return {200, {
        {"metric", metric},
        {"limit", limit},
        {"offset", offset},
        {"entries", std::move(entries)}
    }};
}

std::string AzerothCoreBackend::Hash(std::string const& value) const
{
    return Hex(Acore::Crypto::SHA256::GetDigestOf(value));
}

std::string AzerothCoreBackend::DerivePassword(std::string const& operation,
    std::uint32_t accountId, std::string const& idempotencyKey) const
{
    std::string const material = operation + "\n" + std::to_string(accountId) + "\n" + idempotencyKey;
    Acore::Crypto::HMAC_SHA256 hmac(
        reinterpret_cast<uint8 const*>(_credentialDerivationSecret.data()),
        _credentialDerivationSecret.size());
    hmac.UpdateData(material);
    hmac.Finalize();
    auto const& digest = hmac.GetDigest();
    constexpr char Alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string password;
    password.reserve(16);
    for (std::size_t index = 0; index < 16; ++index)
        password.push_back(Alphabet[digest[index] % (sizeof(Alphabet) - 1)]);
    return password;
}

AzerothCoreBackend::IdempotencyRecord AzerothCoreBackend::Claim(
    std::string const& idempotencyKey, std::string const& operation,
    std::string const& requestFingerprint)
{
    std::string const keyHash = Hash(idempotencyKey);
    LoginDatabase.DirectExecute(
        "INSERT IGNORE INTO ac_api_v1_idempotency "
        "(key_hash, operation, request_hash, state) VALUES ('{}', '{}', '{}', 'pending')",
        keyHash, Escaped(operation), requestFingerprint);

    IdempotencyRecord record;
    QueryResult result = LoginDatabase.Query(
        "SELECT operation, request_hash, state, COALESCE(http_status, 0), "
        "COALESCE(response_json, ''), COALESCE(resource_id, 0) "
        "FROM ac_api_v1_idempotency WHERE key_hash = '{}'", keyHash);
    if (!result)
        return record;

    Field* fields = result->Fetch();
    record.exists = true;
    record.operation = fields[0].Get<std::string>();
    record.requestHash = fields[1].Get<std::string>();
    record.state = fields[2].Get<std::string>();
    record.httpStatus = fields[3].Get<std::uint16_t>();
    std::string const body = fields[4].Get<std::string>();
    if (!body.empty())
        record.response = Json::parse(body, nullptr, false);
    record.resourceId = fields[5].Get<std::uint32_t>();
    return record;
}

Response AzerothCoreBackend::ExistingResult(IdempotencyRecord const& record,
    std::string const& generatedPassword) const
{
    Json body = record.response;
    if (!generatedPassword.empty())
        body["password"] = generatedPassword;
    return {record.httpStatus, std::move(body)};
}

Response AzerothCoreBackend::Complete(std::string const& idempotencyKey,
    std::string const& operation, int status, Json storedBody, std::uint32_t resourceId)
{
    std::string responseJson = storedBody.dump();
    LoginDatabase.EscapeString(responseJson);
    LoginDatabase.DirectExecute(
        "UPDATE ac_api_v1_idempotency SET state = 'completed', http_status = {}, "
        "response_json = '{}', resource_id = {} WHERE key_hash = '{}' AND operation = '{}'",
        status, responseJson, resourceId, Hash(idempotencyKey), Escaped(operation));
    return {status, std::move(storedBody)};
}

void AzerothCoreBackend::Audit(std::string const& eventType, std::uint32_t accountId,
    std::string const& outcomeCode) const
{
    LoginDatabase.Execute(
        "INSERT INTO ac_api_v1_audit_event (event_type, account_id, outcome_code) "
        "VALUES ('{}', {}, '{}')", Escaped(eventType), accountId == 0 ? "NULL" : std::to_string(accountId),
        Escaped(outcomeCode));
}

bool AzerothCoreBackend::IsManaged(std::uint32_t accountId) const
{
    return static_cast<bool>(LoginDatabase.Query(
        "SELECT 1 FROM ac_api_v1_managed_account WHERE account_id = {}", accountId));
}

bool AzerothCoreBackend::IsManagedUsername(std::string const& username,
    std::string const& keyHash) const
{
    std::string escaped = Escaped(username);
    return static_cast<bool>(LoginDatabase.Query(
        "SELECT 1 FROM ac_api_v1_managed_account WHERE normalized_username = '{}' "
        "AND create_key_hash = '{}'", escaped, keyHash))
        || static_cast<bool>(LoginDatabase.Query(
            "SELECT 1 FROM ac_api_v1_account_reservation WHERE normalized_username = '{}' "
            "AND key_hash = '{}'", escaped, keyHash));
}

bool AzerothCoreBackend::ReserveUsername(std::string const& username,
    std::string const& keyHash) const
{
    std::string escaped = Escaped(username);
    LoginDatabase.DirectExecute(
        "INSERT IGNORE INTO ac_api_v1_account_reservation (normalized_username, key_hash) "
        "VALUES ('{}', '{}')", escaped, keyHash);
    return static_cast<bool>(LoginDatabase.Query(
        "SELECT 1 FROM ac_api_v1_account_reservation WHERE normalized_username = '{}' "
        "AND key_hash = '{}'", escaped, keyHash));
}

bool AzerothCoreBackend::RegisterManaged(std::uint32_t accountId, std::string const& username,
    std::string const& keyHash) const
{
    std::string escaped = Escaped(username);
    LoginDatabase.DirectExecute(
        "INSERT IGNORE INTO ac_api_v1_managed_account "
        "(account_id, normalized_username, create_key_hash) VALUES ({}, '{}', '{}')",
        accountId, escaped, keyHash);
    return static_cast<bool>(LoginDatabase.Query(
        "SELECT 1 FROM ac_api_v1_managed_account WHERE account_id = {} "
        "AND normalized_username = '{}' AND create_key_hash = '{}'",
        accountId, escaped, keyHash));
}

bool AzerothCoreBackend::AccountExists(std::uint32_t accountId, std::string& username) const
{
    return AccountMgr::GetName(accountId, username);
}

AzerothCoreBackend::BanState AzerothCoreBackend::GetBanState(std::uint32_t accountId) const
{
    LoginDatabasePreparedStatement* statement =
        LoginDatabase.GetPreparedStatement(LOGIN_SEL_PINFO_BANS);
    statement->SetData(0, accountId);
    PreparedQueryResult result = LoginDatabase.Query(statement);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    std::string const author = fields[2].Get<std::string>();
    std::string const reason = fields[3].Get<std::string>();
    return {true, author == SuspensionAuthor && reason == SuspensionReason};
}

bool AzerothCoreBackend::HasManagedSuspension(std::uint32_t accountId) const
{
    return static_cast<bool>(LoginDatabase.Query(
        "SELECT 1 FROM ac_api_v1_managed_suspension WHERE account_id = {} AND active = 1",
        accountId));
}

Response AzerothCoreBackend::ValidateManagedAccount(std::uint32_t accountId,
    std::string& username) const
{
    if (!AccountExists(accountId, username))
        return Error(404, "account_not_found", "The mapped AzerothCore account no longer exists.");
    if (!IsManaged(accountId))
        return Error(409, "unmanaged_account", "This API will not modify an unmanaged account.");
    if (!AccountMgr::IsPlayerAccount(AccountMgr::GetSecurity(accountId)))
        return Error(409, "privileged_account",
            "This API will not modify a privileged account.");
    return {200, Json::object()};
}
}
