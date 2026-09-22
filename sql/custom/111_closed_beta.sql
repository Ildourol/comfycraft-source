-- Closed beta: a switch, and the accounts allowed in while it is on.
--
-- Apply to tw_logon:
--   Get-Content sql\custom\111_closed_beta.sql | DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_logon
--
-- realmd reads both tables on every logon challenge (AuthSocket.cpp, next to
-- the email verification check), so flipping the switch or editing the list
-- takes effect at the next login with no restart. Accounts with rank >= 1 are
-- always let in, so staff cannot lock themselves out. Edited at /admin/beta.
--
-- Apply this BEFORE the realmd binary that reads it. realmd probes
-- information_schema before its first read and treats a missing table as
-- "beta off", but only an applied table can ever be switched on.
--
-- Re-runnable: creates if missing and never touches existing rows.

CREATE TABLE IF NOT EXISTS `closed_beta` (
  `id`         TINYINT UNSIGNED NOT NULL,
  `enabled`    TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `changed_at` DATETIME NULL,
  `changed_by` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `closed_beta` (`id`, `enabled`) VALUES (1, 0);

CREATE TABLE IF NOT EXISTS `closed_beta_account` (
  `account_id` INT UNSIGNED NOT NULL,
  `added_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `added_by`   VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
