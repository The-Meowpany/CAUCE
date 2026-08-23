import os


class Settings:
    def __init__(self) -> None:
        self.db_path = os.environ.get("CAUCE_DB_PATH", "./data/cauce.sqlite")
        self.sync_token = os.environ.get("CAUCE_SYNC_TOKEN", "")
        self.api_token = os.environ.get("CAUCE_API_TOKEN", "")
        self.rate_limit_per_minute = int(os.environ.get("CAUCE_RATE_LIMIT", "120"))
        self.protocol_version = 1


settings = Settings()
