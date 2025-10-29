-- MariaDB Initialization Script for LuaFan Tests
-- This script runs automatically when the container starts for the first time

-- Create additional databases if needed
CREATE DATABASE IF NOT EXISTS test_db2;
CREATE DATABASE IF NOT EXISTS temp_test_db;

-- Grant all privileges to test user on test databases
GRANT ALL PRIVILEGES ON test_db.* TO 'test_user'@'%';
GRANT ALL PRIVILEGES ON test_db2.* TO 'test_user'@'%';
GRANT ALL PRIVILEGES ON temp_test_db.* TO 'test_user'@'%';

-- Create a test table in test_db for initial verification
USE test_db;

CREATE TABLE IF NOT EXISTS connection_test (
    id INT AUTO_INCREMENT PRIMARY KEY,
    message VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

INSERT INTO connection_test (message) VALUES ('MariaDB is ready for LuaFan tests');

-- Flush privileges to ensure changes take effect
FLUSH PRIVILEGES;