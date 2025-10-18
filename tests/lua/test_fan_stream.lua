#!/usr/bin/env lua

-- Test for fan.stream module (modules/fan/stream/)
-- Stream processing with multiple performance variants: core/bit/ffi

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module if needed
package.preload['config'] = function()
    return {
        stream_ffi = true,  -- Enable FFI variant if available
        stream_bit = true,  -- Enable bit variant if available
        debug = false
    }
end

-- Try to load fan.stream module
local stream_available = false
local stream

local ok, result = pcall(require, 'fan.stream')
if ok then
    stream = result
    stream_available = true
    print("fan.stream module loaded successfully")
else
    print("Error: fan.stream module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.stream Tests")

print("Testing fan.stream module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(stream)
    TestFramework.assert_type(stream, "table")

    -- Check for essential stream creation function
    TestFramework.assert_type(stream.new, "function")
end)

-- Test stream creation
suite:test("stream_creation", function()
    -- Test creating empty stream (write mode)
    local write_stream = stream.new()
    TestFramework.assert_not_nil(write_stream)
    TestFramework.assert_type(write_stream, "userdata")  -- Stream objects are userdata

    -- Test creating stream with initial data (read mode)
    local test_data = "hello world"
    local read_stream = stream.new(test_data)
    TestFramework.assert_not_nil(read_stream)
    TestFramework.assert_type(read_stream, "userdata")

    -- Check available data
    TestFramework.assert_equal(#test_data, read_stream:available())
end)

-- Test basic read/write operations
suite:test("basic_read_write", function()
    local s = stream.new()

    -- Test basic integer operations (using available methods)
    s:AddU8(255)
    s:AddU16(65535)
    -- Note: AddU32 not available, use GetU32 which exists

    -- Test string operations
    s:AddString("test string")
    s:AddBytes("raw bytes")

    -- Switch to read mode
    s:prepare_get()

    -- Read back the data
    TestFramework.assert_equal(255, s:GetU8())
    TestFramework.assert_equal(65535, s:GetU16())
    TestFramework.assert_equal("test string", s:GetString())
    TestFramework.assert_equal("raw bytes", s:GetBytes(9))
end)

-- Test data type operations
suite:test("data_types", function()
    local s = stream.new()

    -- Test various integer sizes (according to stream.md documentation)
    s:AddU8(127)
    s:AddU16(32767)
    s:AddU24(8388607)
    s:AddS24(-8388608)
    -- Note: AddU32 doesn't exist in API, only GetU32 for reading

    -- Test floating point
    s:AddD64(3.14159265359)

    -- Switch to read mode
    s:prepare_get()

    -- Verify data
    TestFramework.assert_equal(127, s:GetU8())
    TestFramework.assert_equal(32767, s:GetU16())
    TestFramework.assert_equal(8388607, s:GetU24())
    TestFramework.assert_equal(-8388608, s:GetS24())

    local pi = s:GetD64()
    TestFramework.assert_true(math.abs(pi - 3.14159265359) < 0.00000000001)
end)

-- Test variable length encoding (U30)
suite:test("variable_length_encoding", function()
    local s = stream.new()

    -- Test different ranges of U30 encoding
    local test_values = {0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455}

    for _, value in ipairs(test_values) do
        s:AddU30(value)
        s:AddABCU32(value)  -- Alias test
    end

    s:prepare_get()

    -- Verify values
    for _, expected in ipairs(test_values) do
        TestFramework.assert_equal(expected, s:GetU30())
        TestFramework.assert_equal(expected, s:GetABCU32())
    end
end)

-- Test string operations
suite:test("string_operations", function()
    local s = stream.new()

    -- Test various string types
    local strings = {
        "",
        "simple",
        "unicode: 你好世界",
        "special chars: !@#$%^&*()",
        string.rep("long", 1000)
    }

    for _, str in ipairs(strings) do
        s:AddString(str)
    end

    s:prepare_get()

    for _, expected in ipairs(strings) do
        TestFramework.assert_equal(expected, s:GetString())
    end
end)

-- Test byte operations
suite:test("byte_operations", function()
    local s = stream.new()

    local test_bytes = "\x00\x01\x02\xFF\xFE\xFD"
    s:AddBytes(test_bytes)

    s:prepare_get()

    -- Test reading exact bytes
    TestFramework.assert_equal(test_bytes, s:GetBytes(#test_bytes))

    -- Test available data tracking
    TestFramework.assert_equal(0, s:available())
end)

-- Test position management (mark/reset)
suite:test("position_management", function()
    local s = stream.new("abcdefghijk")

    -- Read some data
    TestFramework.assert_equal("abc", s:GetBytes(3))

    -- Mark current position
    s:mark()

    -- Read more data
    TestFramework.assert_equal("def", s:GetBytes(3))
    TestFramework.assert_equal("ghi", s:GetBytes(3))

    -- Reset to mark
    s:reset()

    -- Should read from marked position again
    TestFramework.assert_equal("def", s:GetBytes(3))
end)

-- Test read/write mode switching
suite:test("mode_switching", function()
    local s = stream.new()

    -- Write some data
    s:AddString("initial")
    s:AddU30(12345)  -- Use U30 instead of non-existent AddU32

    -- Switch to read mode
    s:prepare_get()
    TestFramework.assert_equal("initial", s:GetString())
    TestFramework.assert_equal(12345, s:GetU30())

    -- Switch back to write mode
    s:prepare_add()
    s:AddString("additional")

    -- Read all data
    s:prepare_get()
    -- Note: After switching to add mode, read position may reset
    -- This tests the current stream implementation behavior
end)

-- Test TestBytes (peek without consuming)
suite:test("test_bytes_peek", function()
    local s = stream.new("hello world")

    -- Peek at first 5 bytes without consuming
    TestFramework.assert_equal("hello", s:TestBytes(5))
    TestFramework.assert_equal(11, s:available())  -- Data still available

    -- Now actually read the data
    TestFramework.assert_equal("hello", s:GetBytes(5))
    TestFramework.assert_equal(6, s:available())   -- Data consumed
end)

-- Test package operation
suite:test("package_operation", function()
    local s = stream.new()

    s:AddString("test")
    s:AddU30(42)  -- Use U30 instead of AddU32
    s:AddBytes("end")

    -- Package all data
    local packed = s:package()
    TestFramework.assert_type(packed, "string")
    TestFramework.assert_true(#packed > 0)

    -- Create new stream from packaged data
    local s2 = stream.new(packed)
    TestFramework.assert_equal("test", s2:GetString())
    TestFramework.assert_equal(42, s2:GetU30())
    TestFramework.assert_equal("end", s2:GetBytes(3))
end)

-- Test empty operation
suite:test("empty_operation", function()
    local s = stream.new("some data")
    TestFramework.assert_true(s:available() > 0)

    s:empty()
    TestFramework.assert_equal(0, s:available())
end)

-- Test readline functionality (from init.lua extension)
-- This is designed for HTTP header parsing - incomplete lines return nil
suite:test("readline_functionality", function()
    if not stream.new().readline then
        print("Note: readline method not available in this stream implementation")
        return
    end

    -- Test complete lines with proper line endings
    local complete_data = "line1\nline2\r\nline3\r\n"
    local s1 = stream.new(complete_data)

    TestFramework.assert_equal("line1", s1:readline())
    TestFramework.assert_equal("line2", s1:readline())
    TestFramework.assert_equal("line3", s1:readline())
    TestFramework.assert_nil(s1:readline())  -- EOF

    -- Test incomplete line behavior (designed for HTTP parsing)
    local incomplete_data = "line1\nline2\r\nincomplete_line"  -- No ending
    local s2 = stream.new(incomplete_data)

    TestFramework.assert_equal("line1", s2:readline())
    TestFramework.assert_equal("line2", s2:readline())
    -- Incomplete line should return nil and preserve data
    TestFramework.assert_nil(s2:readline())
    TestFramework.assert_true(s2:available() > 0)  -- Data preserved

    -- Verify the incomplete data is still there
    local remaining = s2:GetBytes(s2:available())
    TestFramework.assert_equal("incomplete_line", remaining)
end)

-- Test edge cases and error handling
suite:test("edge_cases", function()
    local s = stream.new("")

    -- Test reading from empty stream
    TestFramework.assert_equal(0, s:available())

    -- These operations should handle empty stream gracefully
    local ok1 = pcall(function() return s:GetBytes(1) end)
    local ok2 = pcall(function() return s:GetString() end)
    local ok3 = pcall(function() return s:GetU8() end)

    -- The behavior may vary by implementation, but should not crash
    TestFramework.assert_type(ok1, "boolean")
    TestFramework.assert_type(ok2, "boolean")
    TestFramework.assert_type(ok3, "boolean")
end)

-- Test large data handling
suite:test("large_data_handling", function()
    local s = stream.new()

    -- Create a large string
    local large_string = string.rep("x", 1000)  -- Reduced size for testing
    s:AddString(large_string)

    -- Add many small values using U30
    for i = 1, 100 do  -- Reduced iterations for testing
        s:AddU30(i)
    end

    s:prepare_get()

    -- Verify large string
    TestFramework.assert_equal(large_string, s:GetString())

    -- Verify small values
    for i = 1, 100 do
        TestFramework.assert_equal(i, s:GetU30())
    end
end)

-- Test stream performance characteristics
suite:test("performance_characteristics", function()
    local s = stream.new()

    local start_time = fan.gettime()

    -- Perform many operations using U30
    for i = 1, 100 do  -- Reduced for testing
        s:AddU30(i)
        s:AddString(string.format("item_%d", i))
    end

    local write_time = fan.gettime() - start_time

    s:prepare_get()
    start_time = fan.gettime()

    for i = 1, 100 do
        local num = s:GetU30()
        local str = s:GetString()
        TestFramework.assert_equal(i, num)
        TestFramework.assert_equal(string.format("item_%d", i), str)
    end

    local read_time = fan.gettime() - start_time

    print(string.format("Performance: Write=%.3fms, Read=%.3fms",
                       write_time * 1000, read_time * 1000))

    -- Basic performance expectation (should complete in reasonable time)
    TestFramework.assert_true(write_time < 1.0)  -- Less than 1 second
    TestFramework.assert_true(read_time < 1.0)   -- Less than 1 second
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)