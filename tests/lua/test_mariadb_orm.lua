#!/usr/bin/env lua

-- Test for mariadb.orm module (database ORM with relationship mapping)
-- This module provides comprehensive database operations with object-relational mapping

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module with MariaDB settings for Docker container
package.preload['config'] = function()
    return {
        maria_database = "test_db",
        maria_user = "test_user",
        maria_passwd = "test_password",
        maria_host = "127.0.0.1",
        maria_port = 3306,
        maria_charset = "utf8mb4",
        maria_pool_size = 5,
        debug = false
    }
end

-- Try to load mariadb.orm module
local orm_available = false
local orm

local ok, result = pcall(require, 'mariadb.orm')
if ok then
    orm = result
    orm_available = true
    print("mariadb.orm module loaded successfully")
else
    print("Error: mariadb.orm module not available: " .. tostring(result))
    os.exit(1)
end

-- Try to load mariadb.pool module
local pool_available = false
local pool

local ok2, result2 = pcall(require, 'mariadb.pool')
if ok2 then
    pool = result2
    pool_available = true
    print("mariadb.pool module loaded successfully")
else
    print("Warning: mariadb.pool module not available: " .. tostring(result2))
end

-- Try to connect to MariaDB (Docker container should be running)
local mariadb = require "fan.mariadb"
local config = require "config"

-- Function to check if MariaDB is available
local function check_mariadb_connection()
    local ok, db = pcall(function()
        return mariadb.connect(
            config.maria_database,
            config.maria_user,
            config.maria_passwd,
            config.maria_host,
            config.maria_port
        )
    end)

    if ok and db then
        db:close()
        return true
    else
        return false, db
    end
end

-- Create test suite
local suite = TestFramework.create_suite("mariadb.orm Tests")

print("Testing mariadb.orm module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(orm)
    TestFramework.assert_type(orm, "table")

    -- Check for essential functions
    TestFramework.assert_type(orm.new, "function")
    TestFramework.assert_not_nil(orm.BUILTIN_VALUE_NOW)
    TestFramework.assert_not_nil(orm.FIELD_ID_KEY)
end)

-- Test MariaDB connection (requires Docker container)
suite:test("mariadb_connection", function()
    local is_connected, err = check_mariadb_connection()

    if not is_connected then
        print("Warning: MariaDB not available, skipping database tests")
        print("To run database tests, start MariaDB with Docker:")
        print("docker run -d --name test-mariadb -e MYSQL_ROOT_PASSWORD=root_password \\")
        print("  -e MYSQL_DATABASE=test_db -e MYSQL_USER=test_user \\")
        print("  -e MYSQL_PASSWORD=test_password -p 3306:3306 mariadb:latest")
        return -- Skip remaining database tests
    end

    -- If connection works, test basic database operations
    local db = mariadb.connect(
        config.maria_database,
        config.maria_user,
        config.maria_passwd,
        config.maria_host,
        config.maria_port
    )

    TestFramework.assert_not_nil(db)
    TestFramework.assert_type(db.execute, "function")
    TestFramework.assert_type(db.prepare, "function")
    TestFramework.assert_type(db.close, "function")

    db:close()
    print("MariaDB connection test passed")
end)

-- Test ORM context creation (mock without database)
suite:test("orm_context_creation", function()
    -- Create mock database object for testing
    local mock_db = {
        execute = function(self, sql)
            -- Mock execute function
            if sql:match("show tables") then
                return {
                    fetch = function()
                        return nil -- No tables initially
                    end,
                    close = function() end
                }
            elseif sql:match("CREATE TABLE") then
                return true
            elseif sql:match("show columns") then
                return {
                    fetch = function()
                        return nil -- No columns initially
                    end,
                    close = function() end
                }
            end
            return true
        end,
        prepare = function(self, sql)
            return {
                bind_param = function(...) return true end,
                execute = function() return true end,
                close = function() end,
                fetch = function() return nil end
            }
        end,
        close = function() end,
        getlastautoid = function() return 1 end
    }

    -- Define test models
    local models = {
        users = {
            name = "varchar(100) not null",
            email = "varchar(255) unique",
            created_at = "datetime default current_timestamp"
        },
        posts = {
            title = "varchar(200) not null",
            content = "text",
            user_id = "bigint not null",
            created_at = "datetime default current_timestamp"
        }
    }

    -- Test ORM context creation (this will work without real database)
    local ok, ctx = pcall(orm.new, mock_db, models)
    TestFramework.assert_true(ok)
    TestFramework.assert_not_nil(ctx)
    TestFramework.assert_type(ctx, "table")

    -- Check that models are accessible
    TestFramework.assert_not_nil(ctx.users)
    TestFramework.assert_not_nil(ctx.posts)

    print("ORM context creation test passed")
end)

-- Test ORM model structure
suite:test("orm_model_structure", function()
    local mock_db = {
        execute = function(self, sql)
            if sql:match("show tables") then
                return {
                    fetch = function() return nil end,
                    close = function() end
                }
            elseif sql:match("show columns") then
                return {
                    fetch = function() return nil end,
                    close = function() end
                }
            end
            return true
        end,
        prepare = function() return {bind_param = function() return true end, execute = function() return true end, close = function() end} end,
        close = function() end,
        getlastautoid = function() return 1 end
    }

    local models = {
        test_table = {
            name = "varchar(50)",
            value = "int"
        }
    }

    local ctx = orm.new(mock_db, models)

    -- Check model table structure
    TestFramework.assert_type(ctx.test_table, "table")

    -- Test that model has callable interface
    local mt = getmetatable(ctx.test_table)
    TestFramework.assert_not_nil(mt)
    TestFramework.assert_type(mt.__call, "function")

    print("ORM model structure test passed")
end)

-- Test BUILTIN_VALUE_NOW constant
suite:test("builtin_values", function()
    TestFramework.assert_equal("NOW()", orm.BUILTIN_VALUE_NOW)
    TestFramework.assert_not_nil(orm.FIELD_ID_KEY)

    print("Built-in values test passed")
end)

-- Test MariaDB pool module (if available)
suite:test("mariadb_pool_structure", function()
    if not pool_available then
        print("MariaDB pool module not available, skipping pool tests")
        return
    end

    TestFramework.assert_not_nil(pool)
    TestFramework.assert_type(pool, "table")
    TestFramework.assert_type(pool.new, "function")

    -- Test pool creation (without actual database connection)
    local test_models = {
        test_table = {
            name = "varchar(50)"
        }
    }

    local pool_obj = pool.new(test_models)
    TestFramework.assert_not_nil(pool_obj)
    TestFramework.assert_type(pool_obj, "table")

    -- Check pool methods
    TestFramework.assert_type(pool_obj.pop, "function")
    TestFramework.assert_type(pool_obj.push, "function")
    TestFramework.assert_type(pool_obj.safe, "function")

    print("MariaDB pool structure test passed")
end)

-- Test ORM field operations (mock)
suite:test("orm_field_operations", function()
    local mock_db = {
        execute = function(self, sql)
            if sql:match("show") then
                return {
                    fetch = function() return nil end,
                    close = function() end
                }
            end
            return true
        end,
        prepare = function(self, sql)
            return {
                bind_param = function() return true end,
                execute = function() return true end,
                close = function() end,
                fetch = function()
                    if sql:match("select") then
                        return {id = 1, name = "test", email = "test@example.com"}
                    end
                    return nil
                end
            }
        end,
        close = function() end,
        getlastautoid = function() return 1 end
    }

    local models = {
        users = {
            name = "varchar(100)",
            email = "varchar(255)"
        }
    }

    local ctx = orm.new(mock_db, models)

    -- Test field access
    TestFramework.assert_not_nil(ctx.users.name)
    TestFramework.assert_not_nil(ctx.users.email)

    -- Test field callable interface
    TestFramework.assert_type(ctx.users.name, "table")
    local field_mt = getmetatable(ctx.users.name)
    TestFramework.assert_not_nil(field_mt)
    TestFramework.assert_type(field_mt.__call, "function")

    print("ORM field operations test passed")
end)

-- Test ORM table operations (mock)
suite:test("orm_table_operations", function()
    local mock_db = {
        execute = function(self, sql)
            if sql:match("show") then
                return {
                    fetch = function() return nil end,
                    close = function() end
                }
            end
            return true
        end,
        prepare = function(self, sql)
            return {
                bind_param = function() return true end,
                execute = function() return true end,
                close = function() end,
                fetch = function() return nil end
            }
        end,
        close = function() end,
        getlastautoid = function() return 1 end
    }

    local models = {
        users = {
            name = "varchar(100)",
            email = "varchar(255)"
        }
    }

    local ctx = orm.new(mock_db, models)

    -- Test table callable interface
    local table_mt = getmetatable(ctx.users)
    TestFramework.assert_not_nil(table_mt)
    TestFramework.assert_type(table_mt.__call, "function")

    -- Test select operations (should not crash)
    local ok1, result1 = pcall(function()
        return ctx.users("select")
    end)
    TestFramework.assert_true(ok1)

    -- Test insert operations (should not crash)
    local ok2, result2 = pcall(function()
        return ctx.users("insert", {name = "test", email = "test@example.com"})
    end)
    TestFramework.assert_true(ok2)

    print("ORM table operations test passed")
end)

-- Test error handling (simplified)
suite:test("error_handling", function()
    -- Test with valid mock database and valid models
    local mock_db = {
        execute = function() return {fetch = function() return nil end, close = function() end} end,
        prepare = function() return {bind_param = function() return true end, execute = function() return true end, close = function() end} end,
        close = function() end
    }

    -- Test with valid parameters (should succeed)
    local ok, ctx = pcall(orm.new, mock_db, {test_table = {name = "varchar(50)"}})
    TestFramework.assert_true(ok)
    TestFramework.assert_not_nil(ctx)

    print("Error handling test passed")
end)

-- Test Docker setup instructions
suite:test("docker_setup_info", function()
    print("=== MariaDB Docker Setup Instructions ===")
    print("To test with real MariaDB database, run:")
    print("docker run -d --name test-mariadb \\")
    print("  -e MYSQL_ROOT_PASSWORD=root_password \\")
    print("  -e MYSQL_DATABASE=test_db \\")
    print("  -e MYSQL_USER=test_user \\")
    print("  -e MYSQL_PASSWORD=test_password \\")
    print("  -p 3306:3306 mariadb:latest")
    print("")
    print("Wait for container to be ready, then run tests again")
    print("To stop: docker stop test-mariadb && docker rm test-mariadb")
    print("========================================")

    -- This test always passes, it's just informational
    TestFramework.assert_true(true)
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)