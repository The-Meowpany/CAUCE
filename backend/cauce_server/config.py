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


settings = Settings()
