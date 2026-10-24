CREATE TABLE IF NOT EXISTS ac_api_v1_idempotency (
    key_hash CHAR(64) NOT NULL,
    operation VARCHAR(32) NOT NULL,
    request_hash CHAR(64) NOT NULL,
    state ENUM('pending', 'completed') NOT NULL DEFAULT 'pending',
    http_status SMALLINT UNSIGNED NULL,
    response_json TEXT NULL,
    resource_id INT UNSIGNED NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (key_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS ac_api_v1_account_reservation (
    normalized_username VARCHAR(17) NOT NULL,
    key_hash CHAR(64) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (normalized_username),
    UNIQUE KEY uq_ac_api_v1_reservation_key (key_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS ac_api_v1_managed_account (
    account_id INT UNSIGNED NOT NULL,
    normalized_username VARCHAR(17) NOT NULL,
    create_key_hash CHAR(64) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (account_id),
    UNIQUE KEY uq_ac_api_v1_managed_username (normalized_username),
    UNIQUE KEY uq_ac_api_v1_managed_create_key (create_key_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS ac_api_v1_managed_suspension (
    account_id INT UNSIGNED NOT NULL,
    active TINYINT(1) UNSIGNED NOT NULL DEFAULT 1,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (account_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS ac_api_v1_audit_event (
    event_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    event_type VARCHAR(48) NOT NULL,
    account_id INT UNSIGNED NULL,
    outcome_code VARCHAR(48) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (event_id),
    KEY idx_ac_api_v1_audit_created (created_at),
    KEY idx_ac_api_v1_audit_account (account_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
