#ifndef AC_API_V1_AZEROTHCORE_BACKEND_H
#define AC_API_V1_AZEROTHCORE_BACKEND_H

#include "ApiService.h"

#include <cstdint>
#include <memory>
#include <string>

namespace AcApiV1
{
class CommunityDatabasePool;
class AzerothCoreBackend final : public Backend
{
public:
    AzerothCoreBackend(std::string credentialDerivationSecret,
        std::string communityDatabaseInfo, std::string playerbotsDatabaseName,
        std::uint8_t communityDatabaseThreads);
    ~AzerothCoreBackend() override;
    void KeepAlive();

    bool IsReady() const override;
    Json Capabilities() const override;
    Response InspectAccount(std::string const& username) override;
    Response CreateAccount(std::string const& username, std::string const& password,
        std::string const& idempotencyKey) override;
    Response ResetPassword(std::uint32_t accountId, std::string const& idempotencyKey) override;
    Response SuspendAccount(std::uint32_t accountId, std::string const& idempotencyKey) override;
    Response RestoreAccount(std::uint32_t accountId, std::string const& idempotencyKey) override;
    Response CharacterStatsByName(std::string const& name) override;
    Response CharacterStats(std::uint32_t characterId) override;
    Response GuildStats(std::uint32_t guildId, std::size_t limit, std::size_t offset) override;
    Response RealmStatus(PlayerAudience audience) override;
    Response OnlinePlayers(PlayerAudience audience, std::size_t limit,
        std::size_t offset) override;
    Response PlayerLeaderboard(std::string const& metric, PlayerAudience audience,
        std::size_t limit, std::size_t offset) override;

private:
    struct IdempotencyRecord
    {
        bool exists = false;
        std::string operation;
        std::string requestHash;
        std::string state;
        int httpStatus = 0;
        Json response;
        std::uint32_t resourceId = 0;
    };

    struct BanState
    {
        bool active = false;
        bool managed = false;
    };

    std::string Hash(std::string const& value) const;
    std::string DerivePassword(std::string const& operation, std::uint32_t accountId,
        std::string const& idempotencyKey) const;
    IdempotencyRecord Claim(std::string const& idempotencyKey, std::string const& operation,
        std::string const& requestFingerprint);
    Response ExistingResult(IdempotencyRecord const& record, std::string const& generatedPassword = "") const;
    Response Complete(std::string const& idempotencyKey, std::string const& operation,
        int status, Json storedBody, std::uint32_t resourceId = 0);
    void Audit(std::string const& eventType, std::uint32_t accountId,
        std::string const& outcomeCode) const;
    bool IsManaged(std::uint32_t accountId) const;
    bool IsManagedUsername(std::string const& username, std::string const& keyHash) const;
    bool ReserveUsername(std::string const& username, std::string const& keyHash) const;
    bool RegisterManaged(std::uint32_t accountId, std::string const& username,
        std::string const& keyHash) const;
    bool AccountExists(std::uint32_t accountId, std::string& username) const;
    BanState GetBanState(std::uint32_t accountId) const;
    bool HasManagedSuspension(std::uint32_t accountId) const;
    Response ValidateManagedAccount(std::uint32_t accountId, std::string& username) const;

    std::string _credentialDerivationSecret;
    std::string _playerbotsDatabaseName;
    std::unique_ptr<CommunityDatabasePool> _communityDatabase;
    bool _communityDatabaseReady = false;
};
}
#endif
