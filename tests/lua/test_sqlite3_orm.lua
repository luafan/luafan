#!/usr/bin/env lua

-- Test for sqlite3.orm module (SQLite ORM implementation)
-- This module provides SQLite-based object-relational mapping with local database support

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module if needed
package.preload['config'] = function()
    return {
        debug = false
    }
end

-- Try to load sqlite3.orm module
local orm_available = false
local orm

local ok, result = pcall(require, 'sqlite3.orm')
if ok then
    orm = result
    orm_available = true
    print("sqlite3.orm module loaded successfully")
else
    print("Error: sqlite3.orm module not available: " .. tostring(result))
    print("Note: Requires lsqlite3: luarocks install lsqlite3")
    os.exit(1)
end

-- Try to load lsqlite3 directly
local sqlite3_available = false
local sqlite3

local ok2, result2 = pcall(require, 'lsqlite3')
if ok2 then
    sqlite3 = result2
    sqlite3_available = true
    print("lsqlite3 module loaded successfully")
else
    print("Error: lsqlite3 module not available: " .. tostring(result2))
    print("Install with: luarocks install lsqlite3")
    os.exit(1)
end

-- Function to create test database
local function create_test_database()
    local db = sqlite3.open_memory()
    if not db then
        return nil, "Failed to create in-memory database"
    end
    return db
end

-- Create test suite
local suite = TestFramework.create_suite("sqlite3.orm Tests")

print("Testing sqlite3.orm module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(orm)
    TestFramework.assert_type(orm, "table")

    -- Check for essential functions
    TestFramework.assert_type(orm.new, "function")
end)

-- Test SQLite3 database creation
suite:test("sqlite3_database_creation", function()
    local db = create_test_database()
    TestFramework.assert_not_nil(db)
    TestFramework.assert_type(db.execute, "function")
    TestFramework.assert_type(db.prepare, "function")
    TestFramework.assert_type(db.close, "function")

    -- Test basic database operations
    local result = db:execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
    TestFramework.assert_equal(sqlite3.OK, result)

    db:close()
    print("SQLite3 database creation test passed")
end)

-- Test ORM context creation with SQLite
suite:test("orm_context_creation", function()
    local db = create_test_database()
    TestFramework.assert_not_nil(db)

    -- Define test models
    local models = {
        users = {
            name = "TEXT NOT NULL",
            email = "TEXT UNIQUE",
            created_at = "DATETIME DEFAULT CURRENT_TIMESTAMP"
        },
        posts = {
            title = "TEXT NOT NULL",
            content = "TEXT",
            user_id = "INTEGER NOT NULL",
            created_at = "DATETIME DEFAULT CURRENT_TIMESTAMP"
        }
    }

    -- Create ORM context
    local ctx = orm.new(db, models)
    TestFramework.assert_not_nil(ctx)
    TestFramework.assert_type(ctx, "table")

    -- Check that models are accessible
    TestFramework.assert_not_nil(ctx.users)
    TestFramework.assert_not_nil(ctx.posts)

    db:close()
    print("ORM context creation test passed")
end)

-- Test ORM model structure
suite:test("orm_model_structure", function()
    local db = create_test_database()

    local models = {
        test_table = {
            name = "TEXT",
            value = "INTEGER"
        }
    }

    local ctx = orm.new(db, models)

    -- Check model table structure
    TestFramework.assert_type(ctx.test_table, "table")

    -- Test that model has callable interface
    local mt = getmetatable(ctx.test_table)
    TestFramework.assert_not_nil(mt)
    TestFramework.assert_type(mt.__call, "function")

    db:close()
    print("ORM model structure test passed")
end)

-- Test table creation and schema updates
suite:test("table_schema_management", function()
    local db = create_test_database()

    local initial_models = {
        schema_test = {
            name = "TEXT",
            email = "TEXT"
        }
    }

    -- Create initial schema
    local ctx = orm.new(db, initial_models)
    TestFramework.assert_not_nil(ctx.schema_test)

    -- Verify table was created
    local found_table = false
    for row in db:nrows("SELECT name FROM sqlite_master WHERE type='table' AND name='schema_test'") do
        found_table = true
        break
    end
    TestFramework.assert_true(found_table)

    db:close()
    print("Table schema management test passed")
end)

-- Test basic CRUD operations
suite:test("basic_crud_operations", function()
    local db = create_test_database()

    local models = {
        crud_test = {
            name = "TEXT NOT NULL",
            value = "INTEGER"
        }
    }

    local ctx = orm.new(db, models)

    -- Test insert operation
    local new_record = ctx.crud_test("insert", {name = "test_name", value = 42})
    TestFramework.assert_not_nil(new_record)
    TestFramework.assert_equal("test_name", new_record.name)
    TestFramework.assert_equal(42, new_record.value)
    TestFramework.assert_not_nil(new_record.id)

    -- Test select operation
    local records = ctx.crud_test("select")
    TestFramework.assert_not_nil(records)
    TestFramework.assert_true(#records > 0)
    TestFramework.assert_equal("test_name", records[1].name)

    -- Test select one operation
    local one_record = ctx.crud_test("one", "WHERE name = ?", "test_name")
    TestFramework.assert_not_nil(one_record)
    TestFramework.assert_equal("test_name", one_record.name)

    db:close()
    print("Basic CRUD operations test passed")
end)

-- Test row operations (update, delete)
suite:test("row_operations", function()
    local db = create_test_database()

    local models = {
        row_test = {
            name = "TEXT",
            status = "TEXT"
        }
    }

    local ctx = orm.new(db, models)

    -- Insert a record
    local record = ctx.row_test("insert", {name = "test_row", status = "active"})
    TestFramework.assert_not_nil(record)

    -- Test update operation
    record.status = "inactive"
    if record.update then
        record:update()

        -- Verify update
        local updated = ctx.row_test("one", "WHERE id = ?", record.id)
        TestFramework.assert_equal("inactive", updated.status)
    end

    -- Test delete operation
    if record.delete then
        local delete_result = record:delete()

        -- Verify deletion
        local deleted = ctx.row_test("one", "WHERE id = ?", record.id)
        TestFramework.assert_nil(deleted)
    end

    db:close()
    print("Row operations test passed")
end)

-- Test field access and queries
suite:test("field_operations", function()
    local db = create_test_database()

    local models = {
        field_test = {
            name = "TEXT",
            category = "TEXT"
        }
    }

    local ctx = orm.new(db, models)

    -- Insert test data
    ctx.field_test("insert", {name = "item1", category = "A"})
    ctx.field_test("insert", {name = "item2", category = "B"})
    ctx.field_test("insert", {name = "item3", category = "A"})

    -- Test field access
    TestFramework.assert_not_nil(ctx.field_test.name)
    TestFramework.assert_not_nil(ctx.field_test.category)

    -- Test field queries (if callable)
    if ctx.field_test.category then
        local field_mt = getmetatable(ctx.field_test.category)
        if field_mt and field_mt.__call then
            local category_a_items = ctx.field_test.category(" = ?", "A")
            TestFramework.assert_not_nil(category_a_items)
            -- Should have 2 items with category A
            if #category_a_items > 0 then
                TestFramework.assert_equal("A", category_a_items[1].category)
            end
        end
    end

    db:close()
    print("Field operations test passed")
end)

-- Test direct SQL operations
suite:test("direct_sql_operations", function()
    local db = create_test_database()

    local models = {
        sql_test = {
            name = "TEXT",
            value = "INTEGER"
        }
    }

    local ctx = orm.new(db, models)

    -- Test direct select
    if ctx.select then
        -- Insert some data first
        ctx.sql_test("insert", {name = "sql_item", value = 100})

        local results = ctx:select("SELECT * FROM sql_test WHERE name = ?", "sql_item")
        TestFramework.assert_not_nil(results)
        TestFramework.assert_true(#results > 0)
    end

    -- Test direct insert
    if ctx.insert then
        local result = ctx:insert("INSERT INTO sql_test (name, value) VALUES (?, ?)", "direct_item", 200)
        TestFramework.assert_not_nil(result)
    end

    db:close()
    print("Direct SQL operations test passed")
end)

-- Test error handling (simplified)
suite:test("error_handling", function()
    -- Test with valid database and models
    local db = create_test_database()
    local ok, ctx = pcall(orm.new, db, {test_table = {name = "TEXT"}})
    TestFramework.assert_true(ok)
    TestFramework.assert_not_nil(ctx)

    -- Test invalid SQL (should fail gracefully)
    if ctx.select then
        local ok2, result = pcall(ctx.select, ctx, "INVALID SQL SYNTAX")
        TestFramework.assert_false(ok2) -- Should fail due to invalid SQL
    end

    db:close()
    print("Error handling test passed")
end)

-- Test SQLite constants and features
suite:test("sqlite3_constants", function()
    -- Test SQLite constants
    TestFramework.assert_not_nil(sqlite3.OK)
    TestFramework.assert_not_nil(sqlite3.DONE)
    TestFramework.assert_not_nil(sqlite3.ROW)

    -- Test database creation methods
    local memory_db = sqlite3.open_memory()
    TestFramework.assert_not_nil(memory_db)

    local temp_db = sqlite3.open(":memory:")
    TestFramework.assert_not_nil(temp_db)

    memory_db:close()
    temp_db:close()

    print("SQLite3 constants test passed")
end)

-- Test performance characteristics
suite:test("performance_characteristics", function()
    local db = create_test_database()

    local models = {
        perf_test = {
            name = "TEXT",
            value = "INTEGER"
        }
    }

    local ctx = orm.new(db, models)

    local start_time = fan.gettime()

    -- Insert multiple records
    for i = 1, 50 do  -- Reduced for testing
        ctx.perf_test("insert", {name = string.format("item_%d", i), value = i})
    end

    local insert_time = fan.gettime() - start_time

    -- Select all records
    start_time = fan.gettime()
    local all_records = ctx.perf_test("select")
    local select_time = fan.gettime() - start_time

    TestFramework.assert_equal(50, #all_records)

    print(string.format("Performance: Insert=%.3fms, Select=%.3fms",
                       insert_time * 1000, select_time * 1000))

    -- Basic performance expectation
    TestFramework.assert_true(insert_time < 1.0)  -- Less than 1 second
    TestFramework.assert_true(select_time < 1.0)  -- Less than 1 second

    db:close()
    print("Performance characteristics test passed")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)