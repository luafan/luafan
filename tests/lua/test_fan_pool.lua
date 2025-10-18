#!/usr/bin/env lua

-- Test for fan.pool module (resource pooling)

local TestFramework = require('test_framework')

-- Mock config module if needed (pool uses config.pool_size)
package.preload['config'] = function()
    return {pool_size = 10}
end

-- Try to load fan.pool module
local pool_available = false
local pool

local ok, result = pcall(require, 'fan.pool')
if ok then
    pool = result
    pool_available = true
    print("fan.pool module loaded successfully")
else
    print("Error: fan.pool module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.pool Tests")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(pool)
    TestFramework.assert_type(pool, "table")

    -- Check for essential functions
    TestFramework.assert_type(pool.new, "function")
end)

-- Test basic pool creation
suite:test("basic_pool_creation", function()
    -- Create pool without arguments
    local simple_pool = pool.new()
    TestFramework.assert_not_nil(simple_pool)
    TestFramework.assert_type(simple_pool, "table")

    -- Check pool methods exist
    TestFramework.assert_type(simple_pool.pop, "function")
    TestFramework.assert_type(simple_pool.push, "function")
    TestFramework.assert_type(simple_pool.safe, "function")
end)

-- Test pool with factory function
suite:test("pool_with_factory", function()
    local resource_id = 0

    -- Create pool with onnew factory
    local test_pool = pool.new({
        onnew = function(pool_instance)
            resource_id = resource_id + 1
            return {id = resource_id, type = "test_resource"}
        end
    })

    TestFramework.assert_not_nil(test_pool)

    -- Pop a resource - should create new one via factory
    local resource1 = test_pool:pop()
    TestFramework.assert_not_nil(resource1)
    TestFramework.assert_type(resource1, "table")
    TestFramework.assert_equal(1, resource1.id)
    TestFramework.assert_equal("test_resource", resource1.type)

    -- Push it back
    test_pool:push(resource1)

    -- Pop again - should reuse the existing resource
    local resource2 = test_pool:pop()
    TestFramework.assert_equal(resource1, resource2) -- Same object
end)

-- Test pool with bind/unbind callbacks
suite:test("pool_with_callbacks", function()
    local bind_count = 0
    local unbind_count = 0

    local test_pool = pool.new({
        onnew = function(pool_instance)
            return {value = "resource", state = "clean"}
        end,
        onbind = function(resource, bind_args)
            bind_count = bind_count + 1
            resource.state = "in_use"
            return resource
        end,
        onunbind = function(resource)
            unbind_count = unbind_count + 1
            resource.state = "clean"
            return resource
        end
    })

    local resource = test_pool:pop()
    TestFramework.assert_equal(1, bind_count)
    TestFramework.assert_equal("in_use", resource.state)

    test_pool:push(resource)
    TestFramework.assert_equal(1, unbind_count)
    TestFramework.assert_equal("clean", resource.state)
end)

-- Test safe method for exception handling
suite:test("safe_method", function()
    local test_pool = pool.new({
        onnew = function(pool_instance)
            return {counter = 0}
        end
    })

    -- Test successful operation
    local result = test_pool:safe(function(resource, increment)
        resource.counter = resource.counter + increment
        return resource.counter
    end, 5)

    TestFramework.assert_equal(5, result)

    -- Test that resource is properly returned even after successful operation
    local resource2 = test_pool:pop()
    TestFramework.assert_equal(5, resource2.counter) -- Same resource reused

    test_pool:push(resource2)

    -- Test error handling - resource should still be returned to pool
    local success = test_pool:safe(function(resource)
        resource.counter = resource.counter + 1
        error("test error")
    end)

    -- safe() should return nil on error (as per implementation)
    TestFramework.assert_nil(success)

    -- Verify resource was still returned to pool
    local resource3 = test_pool:pop()
    TestFramework.assert_equal(6, resource3.counter) -- Counter was incremented before error
end)

-- Test multiple resources and pool exhaustion simulation
suite:test("multiple_resources", function()
    local created_count = 0

    -- Create pool that can create up to a few resources
    local test_pool = pool.new({
        onnew = function(pool_instance)
            created_count = created_count + 1
            return {id = created_count, created = true}
        end
    })

    -- Pop multiple resources
    local resources = {}
    for i = 1, 3 do
        resources[i] = test_pool:pop()
        TestFramework.assert_equal(i, resources[i].id)
    end

    TestFramework.assert_equal(3, created_count)

    -- Push them back in different order
    test_pool:push(resources[2])
    test_pool:push(resources[1])
    test_pool:push(resources[3])

    -- Pop again - should reuse existing resources
    local reused1 = test_pool:pop()
    local reused2 = test_pool:pop()

    -- Should be reused resources (implementation uses table.remove, so LIFO order)
    TestFramework.assert_true(reused1.created)
    TestFramework.assert_true(reused2.created)
    TestFramework.assert_equal(3, created_count) -- No new resources created
end)

-- Test onunbind returning nil to remove resource from pool
suite:test("resource_removal", function()
    local resource_create_count = 0

    local test_pool = pool.new({
        onnew = function(pool_instance)
            resource_create_count = resource_create_count + 1
            return {health = "good", uses = 0, id = resource_create_count}
        end,
        onbind = function(resource, bind_args)
            resource.uses = resource.uses + 1
            return resource
        end,
        onunbind = function(resource)
            -- Remove resource if used too many times
            if resource.uses >= 2 then
                return nil -- Remove from pool
            end
            return resource
        end
    })

    -- Use resource twice
    local resource1 = test_pool:pop()
    TestFramework.assert_equal(1, resource1.uses)
    TestFramework.assert_equal(1, resource1.id)
    test_pool:push(resource1)

    local resource2 = test_pool:pop() -- Should be same resource
    TestFramework.assert_equal(resource1, resource2)
    TestFramework.assert_equal(2, resource2.uses)
    test_pool:push(resource2) -- This should remove it from pool (onunbind returns nil)

    -- Next pop should create new resource since the old one was removed
    local resource3 = test_pool:pop()
    TestFramework.assert_not_equal(resource1, resource3)
    TestFramework.assert_equal(1, resource3.uses) -- New resource, onbind called
    TestFramework.assert_equal(2, resource3.id) -- New resource created
    TestFramework.assert_equal(2, resource_create_count) -- Two resources were created
end)

-- Test bind arguments passing
suite:test("bind_arguments", function()
    local received_args = {}

    local test_pool = pool.new({
        onnew = function(pool_instance)
            return {value = "test"}
        end,
        onbind = function(resource, bind_args)
            received_args = bind_args or {}
            return resource
        end
    }, "arg1", "arg2", 42)

    local resource = test_pool:pop()

    -- Verify bind arguments were passed correctly
    TestFramework.assert_type(received_args, "table")
    TestFramework.assert_equal("arg1", received_args[1])
    TestFramework.assert_equal("arg2", received_args[2])
    TestFramework.assert_equal(42, received_args[3])
end)

-- Test pool behavior without factory function
suite:test("pool_without_factory", function()
    local test_pool = pool.new() -- No onnew function

    -- Manually add resources to pool
    test_pool:push("resource1")
    test_pool:push("resource2")

    -- Pop resources
    local resource1 = test_pool:pop()
    local resource2 = test_pool:pop()

    TestFramework.assert_equal("resource2", resource1) -- LIFO order
    TestFramework.assert_equal("resource1", resource2)
end)

-- Test edge cases and error conditions
suite:test("edge_cases", function()
    -- Test pool with only onbind
    local onbind_pool = pool.new({
        onbind = function(resource, bind_args)
            return "processed_" .. tostring(resource)
        end
    })

    onbind_pool:push("raw_resource")
    local processed = onbind_pool:pop()
    TestFramework.assert_equal("processed_raw_resource", processed)

    -- Test pool with only onunbind
    local onunbind_pool = pool.new({
        onunbind = function(resource)
            return "cleaned_" .. tostring(resource)
        end
    })

    onunbind_pool:push("dirty_resource")
    local resource = onunbind_pool:pop()
    TestFramework.assert_equal("cleaned_dirty_resource", resource)
end)

-- Test resource lifecycle
suite:test("resource_lifecycle", function()
    local lifecycle_log = {}

    local test_pool = pool.new({
        onnew = function(pool_instance)
            table.insert(lifecycle_log, "created")
            return {name = "test_resource"}
        end,
        onbind = function(resource, bind_args)
            table.insert(lifecycle_log, "bound")
            return resource
        end,
        onunbind = function(resource)
            table.insert(lifecycle_log, "unbound")
            return resource
        end
    })

    -- Full lifecycle test
    local resource = test_pool:pop() -- Should: create, bind
    test_pool:push(resource)         -- Should: unbind
    local resource2 = test_pool:pop() -- Should: bind (no create)

    TestFramework.assert_equal("created", lifecycle_log[1])
    TestFramework.assert_equal("bound", lifecycle_log[2])
    TestFramework.assert_equal("unbound", lifecycle_log[3])
    TestFramework.assert_equal("bound", lifecycle_log[4])
    TestFramework.assert_equal(4, #lifecycle_log)
end)

-- Test safe method with multiple arguments
suite:test("safe_method_arguments", function()
    local test_pool = pool.new({
        onnew = function(pool_instance)
            return {operations = {}}
        end
    })

    local result = test_pool:safe(function(resource, op, a, b, c)
        table.insert(resource.operations, {op = op, args = {a, b, c}})
        return a + b + c
    end, "add", 1, 2, 3)

    TestFramework.assert_equal(6, result)

    -- Verify the operation was recorded in the resource
    local resource = test_pool:pop()
    TestFramework.assert_equal(1, #resource.operations)
    TestFramework.assert_equal("add", resource.operations[1].op)
    TestFramework.assert_equal(1, resource.operations[1].args[1])
    TestFramework.assert_equal(2, resource.operations[1].args[2])
    TestFramework.assert_equal(3, resource.operations[1].args[3])
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)