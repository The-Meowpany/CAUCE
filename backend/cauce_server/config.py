import os


class Settings:
    def __init__(self) -> None:
        self.db_path = os.environ.get("CAUCE_DB_PATH", "./data/cauce.sqlite")
        self.sync_token = os.environ.get("CAUCE_SYNC_TOKEN", "")
        self.api_token = os.environ.get("CAUCE_API_TOKEN", "")
        self.trust_proxy = os.environ.get("CAUCE_TRUST_PROXY", "") == "1"
        self.sync_require_auth = os.environ.get("CAUCE_SYNC_REQUIRE_AUTH", "") == "1"
        self.rate_limit_per_minute = int(os.environ.get("CAUCE_RATE_LIMIT", "120"))
        self.protocol_version = 1
        self.telegram_bot_token = os.environ.get("CAUCE_TELEGRAM_BOT_TOKEN", "")
        self.ota_releases_path = os.environ.get("CAUCE_OTA_RELEASES", "")
        self.retention_enabled = os.environ.get("CAUCE_RETENTION_ENABLED", "1") == "1"
        self.retention_days = int(os.environ.get("CAUCE_RETENTION_DAYS", "365"))
        self.retention_interval_h = int(
            os.environ.get("CAUCE_RETENTION_INTERVAL_H", "24")
        )
        # How often the server-rendered dashboard reloads itself, in seconds. It was the
        # literal 60 in every page template, which is a reasonable default for a central in
        # a village with one Wi-Fi link and the wrong number for anything else: too slow to
        # watch a node come back, and impossible to turn off without editing the template.
        # 0 disables the meta refresh entirely, for a caller that refreshes another way.
        self.dashboard_refresh_s = int(
            os.environ.get("CAUCE_DASHBOARD_REFRESH_S", "60")
        )
        self.vacuum_interval_h = int(os.environ.get("CAUCE_VACUUM_INTERVAL_H", "24"))
        # The Ed25519 private key that signs node certificates. Empty means "no CA", and
        # every certificate endpoint then refuses rather than falling back to the admin
        # token - see `certs_endpoint`'s note. A fallback would mean the CA was silently
        # optional, which is how a deployment ends up trusting whichever node authenticated
        # last.
        self.ca_private_key = os.environ.get("CAUCE_CA_KEY", "")
        # Certificate lifetime requested at issuance. Bounded by
        # `certificates.MAX_VALIDITY_SECONDS` regardless of what is set here.
        self.cert_validity_seconds = int(
            os.environ.get("CAUCE_CERT_VALIDITY_SECONDS", str(90 * 24 * 3600))
        )


settings = Settings()
