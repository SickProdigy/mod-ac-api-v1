# Security and deployment

`ac-api-v1` is an administrative integration surface. It is designed for a private Docker or WireGuard management path, not direct Internet exposure.

## Network boundary

The module refuses wildcard and public bind addresses. Configure a loopback, RFC1918, `100.64.0.0/10`, or IPv6 unique-local address. Firewall the port so only the calling application can reach it. If traffic leaves the host, use WireGuard or a private TLS reverse proxy; the embedded listener does not terminate TLS.

Health and readiness routes are intentionally unauthenticated and return no sensitive data. All `/v1` routes require a bearer credential.

## Credentials and scopes

Generate three independent random values of at least 32 characters:

- `AccountWriteToken` authorizes account inspection and mutation.
- `CommunityReadToken` authorizes character, guild, realm, and leaderboard reads.
- `CredentialDerivationSecret` recovers generated one-time credentials for safe retries without persisting plaintext passwords.

Do not reuse tokens, commit them, put them in URLs, or log request headers/bodies. Rotating the derivation secret prevents recovery of generated credentials from operations begun with the previous secret.

## Database privilege separation

The worldserver's existing auth connection performs native account operations and writes the module's auth tables. `CommunityDatabaseInfo` must use a separate MySQL principal with read-only access to the characters database. For example, adapt the host and database name before running:

```sql
CREATE USER 'ac_api_community'@'10.%' IDENTIFIED BY 'replace-with-a-long-random-password';
GRANT SELECT ON acore_characters.* TO 'ac_api_community'@'10.%';
FLUSH PRIVILEGES;
```

Do not grant this principal auth or world database access, and do not use the worldserver's broad database credential in `CommunityDatabaseInfo`.

## Ownership and secret handling

Only accounts created and mapped by this API can be reset, suspended, or restored. Username collisions never cause adoption. Restore removes only an active ban whose author and reason match this module and whose ownership row is active.

Plaintext passwords are never stored in module tables or audit rows. A caller-supplied password is accepted only in the request. A generated password appears only in the successful response and deterministic same-key retry. Audit rows contain event type, account ID when known, result code, and timestamp.

Keep idempotency keys confidential enough that they cannot be guessed by unrelated callers; bearer authorization is still required on every retry.

## Operational checks

Use `/healthz` to confirm the listener is alive and `/readyz` to confirm both module auth tables and the read-only community database are reachable. Alert on repeated `401`, `429`, `backend_unavailable`, `partial_success`, and mapping or suspension conflicts. Back up module auth tables with the rest of the auth database.
