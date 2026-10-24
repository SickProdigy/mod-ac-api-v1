# ac-api-v1 development notes

- Keep the module compatible with AzerothCore C++20 and validate changes in an actual AzerothCore worldserver build, not only with standalone mocks.
- Run `./tests/run.sh` for the small routing/validation suite.
- Account mutations use the core auth connection; community reads must stay on the dedicated `CommunityDatabaseInfo` pool intended for a SELECT-only characters-database user.
- Never persist or log plaintext passwords, bearer credentials, derivation secrets, request bodies, or raw backend errors.
- Preserve the invariant that only accounts and suspensions explicitly owned by this API can be modified or removed.
