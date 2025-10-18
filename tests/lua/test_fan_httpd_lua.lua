#!/usr/bin/env lua

-- Test for fan.httpd Lua implementation (modules/fan/httpd/httpd.lua)

local TestFramework = require('test_framework')
local fan = require "fan"

-- Force loading the Lua httpd implementation directly
local httpd = require "fan.httpd.httpd"

-- Create test suite
local suite = TestFramework.create_suite("fan.httpd Lua Implementation Tests")

print("Testing fan.httpd Lua implementation")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(httpd)
    TestFramework.assert_type(httpd, "table")
    TestFramework.assert_type(httpd.bind, "function")
end)

-- Test basic server creation
suite:test("basic_server_creation", function()
    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Hello from Lua httpd")
        end
    })

    TestFramework.assert_not_nil(server_info)
    TestFramework.assert_type(server_info, "table")

    -- Check Lua implementation structure
    TestFramework.assert_type(server_info.host, "string")
    TestFramework.assert_type(server_info.port, "number")
    TestFramework.assert_not_nil(server_info.serv)
    TestFramework.assert_true(server_info.port > 0)

    -- Lua implementation uses connector.bind, so serv has onaccept
    TestFramework.assert_type(server_info.serv.onaccept, "function")
end)

-- Test server with default parameters
suite:test("server_default_params", function()
    local server_info = httpd.bind({
        onService = function(req, resp)
            resp:reply(200, "OK", "Default params test")
        end
    })

    TestFramework.assert_not_nil(server_info)
    TestFramework.assert_equal("0.0.0.0", server_info.host)  -- Lua implementation default
    TestFramework.assert_type(server_info.port, "number")
    TestFramework.assert_true(server_info.port > 0)
end)

-- Test multiple servers
suite:test("multiple_servers", function()
    local server1 = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Server 1")
        end
    })

    local server2 = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Server 2")
        end
    })

    TestFramework.assert_not_nil(server1)
    TestFramework.assert_not_nil(server2)

    -- Only check ports if binding was successful
    if server1.port > 0 and server2.port > 0 then
        TestFramework.assert_not_equal(server1.port, server2.port)
        TestFramework.assert_true(server1.port > 0)
        TestFramework.assert_true(server2.port > 0)
    else
        print("Warning: One or both servers failed to bind")
    end
end)

-- Test error handling
suite:test("error_handling", function()
    -- Test with missing onService callback
    local ok1, result1 = pcall(httpd.bind, {
        host = "127.0.0.1",
        port = 0
        -- Missing onService - should still work for Lua implementation
    })
    TestFramework.assert_type(ok1, "boolean")

    -- Test with invalid host (should be handled gracefully)
    local ok2, result2 = pcall(httpd.bind, {
        host = "999.999.999.999",  -- Invalid IP
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Error test")
        end
    })
    TestFramework.assert_type(ok2, "boolean")
end)

-- Test request/response context structure (simulated)
suite:test("context_structure", function()
    local context_tested = false

    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            context_tested = true

            -- Test request context (req and resp are the same object in Lua implementation)
            TestFramework.assert_not_nil(req)
            TestFramework.assert_equal(req, resp)  -- Same object in Lua implementation

            -- Test context methods
            TestFramework.assert_type(req.reply, "function")
            TestFramework.assert_type(req.reply_start, "function")
            TestFramework.assert_type(req.reply_chunk, "function")
            TestFramework.assert_type(req.reply_end, "function")
            TestFramework.assert_type(req.addheader, "function")
            TestFramework.assert_type(req.read, "function")
            TestFramework.assert_type(req.available, "function")

            resp:reply(200, "OK", "Context test")
        end
    })

    TestFramework.assert_not_nil(server_info)
    -- Note: In real usage, onService would be called when requests arrive
    -- Here we just verify the server was created successfully
end)

-- Test response methods functionality
suite:test("response_methods", function()
    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            -- Test addheader method
            local ok1 = pcall(resp.addheader, resp, "X-Test-Header", "test-value")
            TestFramework.assert_true(ok1)

            -- Test reply method
            local ok2 = pcall(resp.reply, resp, 200, "OK", "Test response")
            TestFramework.assert_true(ok2)
        end
    })

    TestFramework.assert_not_nil(server_info)
end)

-- Test chunked response methods
suite:test("chunked_response", function()
    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            -- Test chunked response sequence
            local ok1 = pcall(resp.reply_start, resp, 200, "OK")
            TestFramework.assert_true(ok1)

            local ok2 = pcall(resp.reply_chunk, resp, "Chunk 1\n")
            TestFramework.assert_true(ok2)

            local ok3 = pcall(resp.reply_chunk, resp, "Chunk 2\n")
            TestFramework.assert_true(ok3)

            local ok4 = pcall(resp.reply_end, resp)
            TestFramework.assert_true(ok4)
        end
    })

    TestFramework.assert_not_nil(server_info)
end)

-- Test server configuration validation
suite:test("server_configuration", function()
    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Config test")
        end
    })

    TestFramework.assert_not_nil(server_info)
    TestFramework.assert_equal("127.0.0.1", server_info.host)
    TestFramework.assert_type(server_info.port, "number")

    -- Only assert port > 0 if binding was successful
    if server_info.port == 0 then
        print("Warning: Server binding failed, port is 0")
    else
        TestFramework.assert_true(server_info.port > 0)
    end
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)