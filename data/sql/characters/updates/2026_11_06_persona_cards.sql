-- A single revision protects all persona slots for one bot. Missing bodies are NULL.
-- Writes insert an absent bot or update by bot_guid and the previous revision.
CREATE TABLE IF NOT EXISTS `mod_ollama_chat_persona_cards` (
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `revision` BIGINT UNSIGNED NOT NULL,
    `accepted_source` TINYINT UNSIGNED NOT NULL COMMENT '0 absent, 1 manual, 2 Claude, 3 Codex',
    `accepted_body` TEXT NULL,
    `claude_alternative` TEXT NULL,
    `codex_alternative` TEXT NULL,
    PRIMARY KEY (`bot_guid`),
    CONSTRAINT `chk_ollama_persona_identity` CHECK (`bot_guid` > 0 AND `revision` > 0),
    CONSTRAINT `chk_ollama_persona_any_slot` CHECK (
        `accepted_body` IS NOT NULL OR `claude_alternative` IS NOT NULL OR `codex_alternative` IS NOT NULL
    ),
    CONSTRAINT `chk_ollama_persona_accepted` CHECK (
        (`accepted_source` = 0 AND `accepted_body` IS NULL) OR
        (`accepted_source` IN (1, 2, 3) AND `accepted_body` IS NOT NULL
            AND OCTET_LENGTH(`accepted_body`) BETWEEN 1 AND 8192)
    ),
    CONSTRAINT `chk_ollama_persona_claude` CHECK (
        `claude_alternative` IS NULL OR OCTET_LENGTH(`claude_alternative`) BETWEEN 1 AND 8192
    ),
    CONSTRAINT `chk_ollama_persona_codex` CHECK (
        `codex_alternative` IS NULL OR OCTET_LENGTH(`codex_alternative`) BETWEEN 1 AND 8192
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
