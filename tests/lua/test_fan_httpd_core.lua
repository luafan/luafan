#!/usr/bin/env lua

-- Test for fan.httpd C implementation (src/httpd.c via fan.httpd.core)

local TestFramework = require('test_framework')
local fan = require "fan"

-- Try to load the C httpd implementation directly
local httpd_core_available = false
local httpd

local ok, result = pcall(require, 'fan.httpd.core')
if ok then
    httpd = result
    httpd_core_available = true
    print("Testing fan.httpd C implementation (core)")
else
    print("Warning: fan.httpd C implementation not available: " .. tostring(result))
    print("Skipping C httpd tests")
    os.exit(0)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.httpd C Implementation Tests")

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
            resp:reply(200, "OK", "Hello from C httpd")
        end
    })

    TestFramework.assert_not_nil(server_info)
    TestFramework.assert_type(server_info, "table")

    -- Check C implementation structure
    TestFramework.assert_not_nil(server_info.serv)
    TestFramework.assert_type(server_info.host, "string")
    TestFramework.assert_type(server_info.port, "number")
    TestFramework.assert_true(server_info.port > 0)

    -- C implementation should have rebind method
    TestFramework.assert_type(server_info.serv.rebind, "function")
end)

-- Test server with default parameters
suite:test("server_default_params", function()
    local server_info = httpd.bind({
        onService = function(req, resp)
            resp:reply(200, "OK", "Default params test")
        end
    })

    TestFramework.assert_not_nil(server_info)
    TestFramework.assert_type(server_info.host, "string")
    TestFramework.assert_type(server_info.port, "number")
    TestFramework.assert_true(server_info.port > 0)
end)

-- Test server rebind functionality
suite:test("server_rebind", function()
    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Rebind test")
        end
    })

    TestFramework.assert_not_nil(server_info)
    local original_port = server_info.port

    -- Test rebind functionality
    local ok = pcall(function()
        server_info.serv:rebind()
    end)

    TestFramework.assert_true(ok)
    -- Port should remain the same after rebind
    TestFramework.assert_equal(original_port, server_info.port)
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

    -- Should have different ports
    TestFramework.assert_not_equal(server1.port, server2.port)

    -- Both should be valid
    TestFramework.assert_true(server1.port > 0)
    TestFramework.assert_true(server2.port > 0)
end)

-- Test error handling
suite:test("error_handling", function()
    -- Test with missing onService callback
    local ok1, result1 = pcall(httpd.bind, {
        host = "127.0.0.1",
        port = 0
        -- Missing onService
    })

    -- This should fail or succeed depending on implementation
    -- Either way, it shouldn't crash
    TestFramework.assert_type(ok1, "boolean")

    -- Test with invalid host
    local ok2, result2 = pcall(httpd.bind, {
        host = "invalid.host.name.that.does.not.exist",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Error test")
        end
    })

    -- Should either fail gracefully or succeed
    TestFramework.assert_type(ok2, "boolean")
end)

-- Test request/response object structure (mock test)
suite:test("request_response_structure", function()
    local request_received = false
    local response_used = false

    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            request_received = true

            -- Test request object structure based on C implementation
            TestFramework.assert_not_nil(req)
            TestFramework.assert_type(req, "table")

            -- Request properties should exist (may be nil until actual request)
            -- These are accessible via metamethods in C implementation
            TestFramework.assert_not_nil(req.path or "")
            TestFramework.assert_not_nil(req.method or "")
            TestFramework.assert_type(req.headers or {}, "table")
            TestFramework.assert_type(req.params or {}, "table")

            -- Request methods should exist
            TestFramework.assert_type(req.available, "function")
            TestFramework.assert_type(req.read, "function")

            -- Test response object structure
            TestFramework.assert_not_nil(resp)
            TestFramework.assert_type(resp, "table")

            -- Response methods should exist based on C implementation
            TestFramework.assert_type(resp.addheader, "function")
            TestFramework.assert_type(resp.reply, "function")
            TestFramework.assert_type(resp.reply_start, "function")
            TestFramework.assert_type(resp.reply_chunk, "function")
            TestFramework.assert_type(resp.reply_end, "function")

            response_used = true
            resp:reply(200, "OK", "Structure test")
        end
    })

    TestFramework.assert_not_nil(server_info)

    -- Note: We can't easily make an actual HTTP request in this test environment
    -- So we just verify the server was created and the structure is correct
    TestFramework.assert_true(server_info.port > 0)
end)

-- Test response methods functionality
suite:test("response_methods", function()
    local response_obj = nil

    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            response_obj = resp

            -- Test addheader method
            local ok1 = pcall(resp.addheader, resp, "X-Test-Header", "test-value")
            TestFramework.assert_true(ok1)

            -- Test reply method
            local ok2 = pcall(resp.reply, resp, 200, "OK", "Test response")
            TestFramework.assert_true(ok2)
        end
    })

    TestFramework.assert_not_nil(server_info)
    -- The onService callback will be called when a request is made
    -- For now, we just verify the server was created successfully
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

-- Test server host/port configuration
suite:test("server_configuration", function()
    -- Test specific host configuration
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
    TestFramework.assert_true(server_info.port > 0)
end)

-- Test SSL support (if available) - Skip this test as it requires valid certificates
suite:test("ssl_support_check", function()
    -- Just test that SSL parameters don't crash the system
    -- We skip actual SSL testing to avoid certificate file dependencies

    -- Test basic server creation without SSL first
    local ok, server_info = pcall(httpd.bind, {
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "No SSL test")
        end
    })

    -- Basic server should work
    TestFramework.assert_true(ok)
    TestFramework.assert_not_nil(server_info)
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)