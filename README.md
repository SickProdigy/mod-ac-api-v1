# ac-api-v1

A private HTTP API module for [AzerothCore](https://www.azerothcore.org/) that provides managed player-account lifecycle operations and read-only community data for websites and Discord bots.

## Features

- Version and capability discovery.
- Inspect, idempotently create, reset, suspend, and restore API-managed non-GM accounts.
- Human-by-default player leaderboards and online-player lists, with explicit AI or mixed audiences.
- Character, guild roster, and realm population endpoints.
- Separate bearer credentials for account writes and community reads.
- Persistent idempotency, managed-account ownership, managed-suspension ownership, and secret-free audit events.
- Private-address-only listener with bounded payloads, queueing, timeouts, pagination, and rate limiting.
- A dedicated community database pool intended for a `SELECT`-only MySQL user.

The API will not adopt or modify pre-existing unmanaged accounts, privileged accounts, or bans it did not create.

## Installation

Place this repository at `modules/ac-api-v1` in an AzerothCore checkout, then reconfigure and rebuild the worldserver. AzerothCore discovers the module automatically and installs `ac_api_v1.conf.dist` with the other module configuration templates.

The auth database migration in `data/sql/db-auth/base_ac_api_v1.sql` is loaded through AzerothCore's module SQL system. It creates only module-owned bookkeeping and audit tables.

## Configuration

Copy the installed module configuration template to its active `.conf` form and set:

- three distinct, random secrets of at least 32 characters;
- a loopback, RFC1918, CGNAT, or IPv6 unique-local bind address;
- `CommunityDatabaseInfo` for a `SELECT`-only user on `acore_characters`;
- `PlayerbotsDatabaseName` for the schema containing `playerbots_account_type`, with that same user granted `SELECT` on this table;
- `AcApiV1.Enable = 1` after the database grants and private network path are ready.

See [SECURITY.md](SECURITY.md) for the recommended grants and network boundary. See [docs/API.md](docs/API.md) for the endpoint contract and failure semantics.

## Development

Run the standalone routing and validation suite:

```bash
./tests/run.sh
```

For authoritative integration validation, build an AzerothCore worldserver with this directory installed as `modules/ac-api-v1`. The module targets C++20 and has been checked with AzerothCore's warnings enabled.

Vendored libraries are pinned under `deps/`; their upstream licenses are retained in each dependency directory.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
