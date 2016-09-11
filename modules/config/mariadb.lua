maria_host = os.getenv("MARIA_HOST") or os.getenv("MARIA_PORT_3306_TCP_ADDR") or "127.0.0.1"
maria_port = os.getenv("MARIA_PORT") or os.getenv("MARIA_PORT_3306_TCP_PORT")
maria_database = os.getenv("MARIA_DATABASE_NAME") or "test"
maria_user = os.getenv("MARIA_USERNAME") or "root"
maria_passwd = os.getenv("MARIA_PASSWORD") or os.getenv("MARIA_ENV_MYSQL_ROOT_PASSWORD")
maria_charset = os.getenv("MARIA_CHARSET") or "utf8"

maria_pool_count = tonumber(os.getenv("MARIA_POOL_COUNT") or 10)
