# API v1 contract

The service returns JSON. Successful and error responses send `Cache-Control: no-store`; errors use:

```json
{"error":{"code":"stable_code","message":"Human-readable explanation."}}
```

Send `Authorization: Bearer <token>`. Account routes require the account-write token; community routes require the community-read token. Mutating calls also require a 16-128 character visible-ASCII `Idempotency-Key`.

## Health and discovery

- `GET /healthz` — listener liveness; public.
- `GET /readyz` — auth bookkeeping and community database readiness; public.
- `GET /v1/capabilities` — version, core revision, supported operations, community resources, and leaderboard metrics; either token.

## Managed accounts

- `GET /v1/accounts/by-username/{username}` — normalized username, stable account ID, managed flag, bounded state, and whether it is a player account.
- `POST /v1/accounts` — idempotently create a non-GM account. Body: `{"username":"PLAYER","password":"optional8to16"}`. Omitting `password` generates one and returns it only on success.
- `POST /v1/accounts/{id}/password-reset` — generate and set a replacement password without the old password.
- `POST /v1/accounts/{id}/suspend` — apply a native permanent account ban owned by this module.
- `POST /v1/accounts/{id}/restore` — remove only an active module-owned suspension.

Usernames contain 1-17 ASCII letters or digits and are normalized to uppercase. Passwords contain 8-16 printable ASCII characters.

### Idempotency and recovery

An idempotency key is bound to one operation and request fingerprint. Reusing it for different input returns `idempotency_conflict`. Completed retries return the recorded status and response. Generated credentials are reproduced from the stable derivation secret and key, so plaintext is not persisted.

Creation reserves the normalized username before invoking AzerothCore. If the native create succeeds but its row is not visible before the bounded wait expires, the API returns `partial_success`; retry the identical request with the same key. A retry reconciles only the account matching the API reservation and never adopts an unmanaged collision. Pending rows remain retryable after a process crash.

## Community reads

- `GET /v1/characters/{id}` — public character profile statistics and guild summary.
- `GET /v1/guilds/{id}?limit=25&offset=0` — guild summary and paginated roster.
- `GET /v1/realm/status` — total and currently online active characters.
- `GET /v1/leaderboards/players?metric=level&limit=25&offset=0` — ranked active characters. Metrics: `level`, `playtime`, `honor`, `kills`.

`limit` defaults to 25 and is capped by `MaxPageSize`; `offset` defaults to zero. Malformed or signed pagination values are rejected.

## Stable failure codes

Common codes include `unauthorized`, `not_found`, `request_too_large`, `rate_limited`, `invalid_json`, `invalid_request`, `invalid_username`, `invalid_password`, `invalid_account_id`, `invalid_character_id`, `invalid_guild_id`, `invalid_idempotency_key`, `invalid_pagination`, `invalid_metric`, `account_not_found`, `character_not_found`, `guild_not_found`, `account_collision`, `idempotency_conflict`, `unmanaged_account`, `privileged_account`, `mapping_conflict`, `suspension_conflict`, `not_api_suspended`, `partial_success`, `backend_error`, and `backend_unavailable`.

Database and native exception text is never placed in an API response. Unsupported routes and operations return `not_found`. Native suspend/restore support is advertised in capabilities rather than silently emulated.

## Limits

The listener enforces configured body size, URI/header limits, worker and queue bounds, socket read/write/keep-alive timeouts, maximum page size, and per-client request rate. Database connection behavior is additionally bounded by the AzerothCore/MySQL connection settings in `CommunityDatabaseInfo`.
