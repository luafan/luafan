#!/usr/bin/env lua

-- Test for fan.orm_base — adapter pattern with mock backend.
-- Tests the shared ORM logic (row_mt, field_mt, table_mt, ctx) without
-- requiring any real database.

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local orm_base = require 'fan.orm_base'

local suite = TestFramework.create_suite("ORM Base (adapter pattern)")

-- Build a mock adapter that records calls and returns scripted results.
-- This lets us test orm_base logic in isolation.
local function make_mock_adapter(opts)
    opts = opts or {}
    local adapter = {}

    adapter.BUILTIN_VALUE_NOW = opts.builtin_now or nil
    adapter.FIELD_ID_KEY = opts.field_id_key or nil
    adapter.limit_one = opts.limit_one or " limit 1"
    adapter.readonly_supported = opts.readonly or false
    adapter.insert_handle_long_data = false

    local exec_log = {}

    function adapter.create_ctx()
        return {}
    end

    function adapter.prepare(db, sql)
        table.insert(exec_log, { op = "prepare", sql = sql })
        return { sql = sql, bound = {} }
    end

    function adapter.bind_values(stmt, ...)
        table.insert(exec_log, { op = "bind", args = {...} })
        stmt.bound = {...}
    end

    function adapter.execute_stmt(stmt)
        table.insert(exec_log, { op = "execute", sql = stmt.sql })
        return "ok"
    end

    function adapter.fetch_rows(t, stmt, make_row_mt)
        table.insert(exec_log, { op = "fetch_rows", sql = stmt.sql })
        local rows = opts.fetch_result or {}
        local lines = {}
        for _, raw in ipairs(rows) do
            local r = {}
            local attr = {}
            for k, v in pairs(raw) do
                r[k] = v
                attr[k] = v
            end
            r[orm_base.KEY_ATTR] = attr
            setmetatable(r, make_row_mt(t))
            table.insert(lines, r)
        end
        return lines
    end

    function adapter.each_rows(t, stmt, func, make_row_mt)
        local rows = opts.fetch_result or {}
        for _, raw in ipairs(rows) do
            local r = {}
            local attr = {}
            for k, v in pairs(raw) do
                r[k] = v
                attr[k] = v
            end
            r[orm_base.KEY_ATTR] = attr
            setmetatable(r, make_row_mt(t))
            if func(r) then break end
        end
    end

    function adapter.close_stmt(stmt)
        table.insert(exec_log, { op = "close" })
    end

    function adapter.delete_row(db, tablename, field_id, id_value)
        table.insert(exec_log, { op = "delete_row", table = tablename, id = id_value })
        return "ok"
    end

    function adapter.get_last_id(db)
        return opts.last_id or 1
    end

    function adapter.update_schema(ctx, db, tablename, model)
        table.insert(exec_log, { op = "update_schema", table = tablename })
    end

    function adapter.ctx_select_rows(ctx, db, stmt)
        table.insert(exec_log, { op = "ctx_select", sql = stmt.sql })
        return opts.ctx_result or {}
    end

    function adapter.ctx_exec(ctx, db, stmt)
        table.insert(exec_log, { op = "ctx_exec", sql = stmt.sql })
        return "ok"
    end

    adapter._log = exec_log
    return adapter
end

-- Helper: create an ORM context with a mock adapter
local function make_ctx(adapter_opts, models)
    local adapter = make_mock_adapter(adapter_opts)
    local mod = orm_base.create(adapter)
    local db = {} -- mock db handle
    local ctx = mod.new(db, models)
    return ctx, adapter, db
end

-- Test: module structure
suite:test("module_structure", function()
    TestFramework.assert_type(orm_base, "table")
    TestFramework.assert_type(orm_base.create, "function")
    TestFramework.assert_type(orm_base.KEY_CONTEXT, "string")
    TestFramework.assert_type(orm_base.KEY_TABLE, "string")
    TestFramework.assert_type(orm_base.KEY_ATTR, "string")
    TestFramework.assert_type(orm_base.KEY_MODEL, "string")
    TestFramework.assert_type(orm_base.KEY_NAME, "string")
    TestFramework.assert_equal("id", orm_base.FIELD_ID_DEFAULT)
end)

-- Test: create returns a module with new function
suite:test("create_returns_module", function()
    local adapter = make_mock_adapter()
    local mod = orm_base.create(adapter)
    TestFramework.assert_type(mod, "table")
    TestFramework.assert_type(mod.new, "function")
end)

-- Test: new creates context with model tables
suite:test("new_creates_context", function()
    local ctx, adapter = make_ctx({}, {
        users = { name = "TEXT", email = "TEXT" },
        posts = { title = "TEXT" },
    })

    TestFramework.assert_not_nil(ctx)
    TestFramework.assert_not_nil(ctx.users)
    TestFramework.assert_not_nil(ctx.posts)

    -- Should have called update_schema for each table
    local schemas = {}
    for _, entry in ipairs(adapter._log) do
        if entry.op == "update_schema" then
            table.insert(schemas, entry.table)
        end
    end
    table.sort(schemas)
    TestFramework.assert_equal("posts", schemas[1])
    TestFramework.assert_equal("users", schemas[2])
end)

-- Test: table select returns rows
suite:test("table_select", function()
    local ctx, adapter = make_ctx({
        fetch_result = {
            { id = 1, name = "alice", email = "a@b.com" },
            { id = 2, name = "bob", email = "b@c.com" },
        }
    }, {
        users = { name = "TEXT", email = "TEXT" },
    })

    local rows = ctx.users("select")
    TestFramework.assert_not_nil(rows)
    TestFramework.assert_equal(2, #rows)
    TestFramework.assert_equal("alice", rows[1].name)
    TestFramework.assert_equal("bob", rows[2].name)
end)

-- Test: table select one returns single row or nil
suite:test("table_select_one", function()
    local ctx_single = make_ctx({
        fetch_result = { { id = 1, name = "alice" } }
    }, {
        users = { name = "TEXT" },
    })

    local row = ctx_single.users("one")
    TestFramework.assert_not_nil(row)
    TestFramework.assert_equal("alice", row.name)

    local ctx_empty = make_ctx({ fetch_result = {} }, {
        users = { name = "TEXT" },
    })
    local no_row = ctx_empty.users("one")
    TestFramework.assert_nil(no_row)
end)

-- Test: table select with where clause and params
suite:test("table_select_where", function()
    local ctx, adapter = make_ctx({
        fetch_result = { { id = 1, name = "alice" } }
    }, {
        users = { name = "TEXT" },
    })

    local rows = ctx.users("select", "WHERE name = ?", "alice")
    TestFramework.assert_equal(1, #rows)

    -- Verify prepare was called with the where clause
    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "prepare" and entry.sql:match("WHERE name = %?") then
            found = true
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: table insert
suite:test("table_insert", function()
    local ctx, adapter = make_ctx({ last_id = 42 }, {
        users = { name = "TEXT", email = "TEXT" },
    })

    local row = ctx.users("insert", { name = "alice", email = "a@b.com" })
    TestFramework.assert_not_nil(row)
    TestFramework.assert_equal("alice", row.name)
    TestFramework.assert_equal("a@b.com", row.email)
    TestFramework.assert_equal(42, row.id)
end)

-- Test: table delete
suite:test("table_delete", function()
    local ctx, adapter = make_ctx({}, {
        users = { name = "TEXT" },
    })

    ctx.users("delete", "WHERE id = ?", 5)

    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "prepare" and entry.sql:match("delete from users") then
            found = true
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: row update detects changed fields
suite:test("row_update", function()
    local ctx, adapter = make_ctx({
        fetch_result = { { id = 1, name = "alice", email = "a@b.com" } }
    }, {
        users = { name = "TEXT", email = "TEXT" },
    })

    local rows = ctx.users("select")
    local row = rows[1]

    -- Original values match attr
    TestFramework.assert_equal("alice", row.name)

    -- Change a field
    row.name = "bob"
    row:update()

    -- Verify update SQL was issued
    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "prepare" and entry.sql:match("update users set") then
            found = true
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: row delete
suite:test("row_delete", function()
    local ctx, adapter = make_ctx({
        fetch_result = { { id = 7, name = "alice" } }
    }, {
        users = { name = "TEXT" },
    })

    local rows = ctx.users("select")
    local row = rows[1]
    local result = row:delete()

    TestFramework.assert_not_nil(result)

    -- Verify delete_row was called
    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "delete_row" and entry.id == 7 then
            found = true
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: field access creates field objects
suite:test("field_access", function()
    local ctx = make_ctx({}, {
        users = { name = "TEXT", email = "TEXT" },
    })

    -- Accessing a model field should return a field object
    local name_field = ctx.users.name
    TestFramework.assert_not_nil(name_field)

    local mt = getmetatable(name_field)
    TestFramework.assert_not_nil(mt)
    TestFramework.assert_type(mt.__call, "function")
end)

-- Test: field query
suite:test("field_query", function()
    local ctx, adapter = make_ctx({
        fetch_result = { { id = 1, name = "alice" } }
    }, {
        users = { name = "TEXT" },
    })

    local rows = ctx.users.name(" = ?", "alice")
    TestFramework.assert_not_nil(rows)
    TestFramework.assert_equal(1, #rows)
    TestFramework.assert_equal("alice", rows[1].name)
end)

-- Test: ctx.raw_select for direct SQL
suite:test("ctx_raw_select", function()
    local ctx, adapter = make_ctx({
        ctx_result = { { count = 5 } }
    }, {
        users = { name = "TEXT" },
    })

    local results = ctx:select("SELECT count(*) as count FROM users")
    TestFramework.assert_not_nil(results)
    TestFramework.assert_equal(5, results[1].count)
end)

-- Test: ctx.raw_insert for direct SQL
suite:test("ctx_raw_insert", function()
    local ctx, adapter = make_ctx({}, {
        users = { name = "TEXT" },
    })

    ctx:insert("INSERT INTO users (name) VALUES (?)", "alice")

    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "ctx_exec" and entry.sql:match("INSERT") then
            found = true
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: ctx.raw_update/delete for direct SQL
suite:test("ctx_raw_update_delete", function()
    local ctx, adapter = make_ctx({}, {
        users = { name = "TEXT" },
    })

    ctx:update("UPDATE users SET name = ? WHERE id = ?", "bob", 1)
    ctx:delete("DELETE FROM users WHERE id = ?", 1)

    local update_found = false
    local delete_found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "ctx_exec" and entry.sql:match("UPDATE") then
            update_found = true
        end
        if entry.op == "ctx_exec" and entry.sql:match("DELETE") then
            delete_found = true
        end
    end
    TestFramework.assert_true(update_found)
    TestFramework.assert_true(delete_found)
end)

-- Test: BUILTIN_VALUE_NOW passthrough
suite:test("builtin_now_passthrough", function()
    local adapter = make_mock_adapter({ builtin_now = "NOW()" })
    local mod = orm_base.create(adapter)
    TestFramework.assert_equal("NOW()", mod.BUILTIN_VALUE_NOW)
end)

-- Test: FIELD_ID_KEY passthrough
suite:test("field_id_key_passthrough", function()
    local key = {}
    local adapter = make_mock_adapter({ field_id_key = key })
    local mod = orm_base.create(adapter)
    TestFramework.assert_equal(key, mod.FIELD_ID_KEY)
end)

-- Test: each_rows with callback
suite:test("each_rows_callback", function()
    local collected = {}
    local adapter = make_mock_adapter({
        fetch_result = {
            { id = 1, name = "alice" },
            { id = 2, name = "bob" },
            { id = 3, name = "carol" },
        }
    })
    local mod = orm_base.create(adapter)
    local db = {}
    local ctx = mod.new(db, { users = { name = "TEXT" } })

    -- Use table as function (callback pattern)
    ctx.users(function(row)
        table.insert(collected, row.name)
    end)

    TestFramework.assert_equal(3, #collected)
    TestFramework.assert_equal("alice", collected[1])
    TestFramework.assert_equal("bob", collected[2])
    TestFramework.assert_equal("carol", collected[3])
end)

-- Test: insert with nil values skipped
suite:test("insert_nil_values_skipped", function()
    local ctx, adapter = make_ctx({ last_id = 1 }, {
        users = { name = "TEXT", email = "TEXT" },
    })

    -- Insert with only name, email is nil
    local row = ctx.users("insert", { name = "alice" })
    TestFramework.assert_not_nil(row)

    -- The INSERT should only have name column, not email
    local found = false
    for _, entry in ipairs(adapter._log) do
        if entry.op == "prepare" and entry.sql:match("insert") then
            if entry.sql:match("name") and not entry.sql:match("email") then
                found = true
            end
        end
    end
    TestFramework.assert_true(found)
end)

-- Test: table mt __index caches field objects
suite:test("field_caching", function()
    local ctx = make_ctx({}, {
        users = { name = "TEXT" },
    })

    local f1 = ctx.users.name
    local f2 = ctx.users.name
    -- Should be the same cached object
    TestFramework.assert_equal(f1, f2)
end)

-- Run
local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
