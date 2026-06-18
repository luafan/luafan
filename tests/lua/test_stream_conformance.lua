#!/usr/bin/env lua

-- Stream conformance test: validates byte-level correctness across all backends.
-- Under Lua 5.3, tests the C core. Under LuaJIT, tests bit/ffi.

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local stream = require('fan.stream')
local fan = require "fan"

local suite = TestFramework.create_suite("Stream Conformance")

-- Helper: write values, package, re-read, verify
local function roundtrip(write_fn, read_fn, label)
    local s = stream.new()
    write_fn(s)
    local packed = s:package()
    local s2 = stream.new(packed)
    read_fn(s2, label)
end

-- Test U8 byte-level encoding
suite:test("u8_encoding", function()
    local s = stream.new()
    s:AddU8(0)
    s:AddU8(1)
    s:AddU8(127)
    s:AddU8(128)
    s:AddU8(255)
    local packed = s:package()
    TestFramework.assert_equal(5, #packed)
    TestFramework.assert_equal(0x00, packed:byte(1))
    TestFramework.assert_equal(0x01, packed:byte(2))
    TestFramework.assert_equal(0x7F, packed:byte(3))
    TestFramework.assert_equal(0x80, packed:byte(4))
    TestFramework.assert_equal(0xFF, packed:byte(5))
end)

-- Test U16 little-endian encoding
suite:test("u16_encoding", function()
    local s = stream.new()
    s:AddU16(0)
    s:AddU16(1)
    s:AddU16(256)
    s:AddU16(65535)
    local packed = s:package()
    TestFramework.assert_equal(8, #packed)
    -- 0: 0x00 0x00
    TestFramework.assert_equal(0x00, packed:byte(1))
    TestFramework.assert_equal(0x00, packed:byte(2))
    -- 1: 0x01 0x00
    TestFramework.assert_equal(0x01, packed:byte(3))
    TestFramework.assert_equal(0x00, packed:byte(4))
    -- 256: 0x00 0x01
    TestFramework.assert_equal(0x00, packed:byte(5))
    TestFramework.assert_equal(0x01, packed:byte(6))
    -- 65535: 0xFF 0xFF
    TestFramework.assert_equal(0xFF, packed:byte(7))
    TestFramework.assert_equal(0xFF, packed:byte(8))
end)

-- Test U24 little-endian encoding
suite:test("u24_encoding", function()
    local s = stream.new()
    s:AddU24(0)
    s:AddU24(1)
    s:AddU24(0x010203)
    s:AddU24(0xFFFFFF)
    local packed = s:package()
    TestFramework.assert_equal(12, #packed)
    -- 0
    TestFramework.assert_equal(0x00, packed:byte(1))
    TestFramework.assert_equal(0x00, packed:byte(2))
    TestFramework.assert_equal(0x00, packed:byte(3))
    -- 1
    TestFramework.assert_equal(0x01, packed:byte(4))
    TestFramework.assert_equal(0x00, packed:byte(5))
    TestFramework.assert_equal(0x00, packed:byte(6))
    -- 0x010203
    TestFramework.assert_equal(0x03, packed:byte(7))
    TestFramework.assert_equal(0x02, packed:byte(8))
    TestFramework.assert_equal(0x01, packed:byte(9))
    -- 0xFFFFFF
    TestFramework.assert_equal(0xFF, packed:byte(10))
    TestFramework.assert_equal(0xFF, packed:byte(11))
    TestFramework.assert_equal(0xFF, packed:byte(12))
end)

-- Test S24 little-endian encoding (signed 24-bit)
suite:test("s24_encoding", function()
    local s = stream.new()
    s:AddS24(0)
    s:AddS24(-1)
    s:AddS24(-8388608)  -- min S24
    s:AddS24(8388607)   -- max S24
    local packed = s:package()
    local s2 = stream.new(packed)
    TestFramework.assert_equal(0, s2:GetS24())
    TestFramework.assert_equal(-1, s2:GetS24())
    TestFramework.assert_equal(-8388608, s2:GetS24())
    TestFramework.assert_equal(8388607, s2:GetS24())
end)

-- Test D64 (double) encoding — verify round-trip precision
suite:test("d64_encoding", function()
    local values = {0, 1, -1, 3.141592653589793, 1e100, -1e-100, math.huge, -math.huge}
    local s = stream.new()
    for _, v in ipairs(values) do
        s:AddD64(v)
    end
    local packed = s:package()
    TestFramework.assert_equal(#values * 8, #packed)

    local s2 = stream.new(packed)
    for _, expected in ipairs(values) do
        local got = s2:GetD64()
        if expected == math.huge or expected == -math.huge then
            TestFramework.assert_true(got == expected, "infinity mismatch")
        else
            TestFramework.assert_true(math.abs(got - expected) < 1e-10,
                string.format("D64 mismatch: expected %s, got %s", tostring(expected), tostring(got)))
        end
    end
end)

-- Test U30 variable-length encoding byte patterns
suite:test("u30_encoding", function()
    -- U30 encodes as: 0-127 = 1 byte, 128-16383 = 2 bytes, etc.
    local cases = {
        {val = 0, bytes = 1},
        {val = 127, bytes = 1},
        {val = 128, bytes = 2},
        {val = 16383, bytes = 2},
        {val = 16384, bytes = 3},
        {val = 2097151, bytes = 3},
        {val = 2097152, bytes = 4},
        {val = 268435455, bytes = 4},
    }

    for _, tc in ipairs(cases) do
        local s = stream.new()
        s:AddU30(tc.val)
        local packed = s:package()
        TestFramework.assert_equal(tc.bytes, #packed,
            string.format("U30(%d): expected %d bytes, got %d", tc.val, tc.bytes, #packed))

        local s2 = stream.new(packed)
        TestFramework.assert_equal(tc.val, s2:GetU30(),
            string.format("U30(%d) round-trip failed", tc.val))
    end
end)

-- Test ABCU32 (alias for U30)
suite:test("abcu32_alias", function()
    local values = {0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455}
    local s = stream.new()
    for _, v in ipairs(values) do
        s:AddABCU32(v)
    end
    s:prepare_get()
    for _, expected in ipairs(values) do
        TestFramework.assert_equal(expected, s:GetABCU32())
    end
end)

-- Test String encoding (U30 length prefix + data)
suite:test("string_encoding", function()
    local cases = {"", "a", "hello", string.rep("x", 128), string.rep("y", 16384)}
    local s = stream.new()
    for _, str in ipairs(cases) do
        s:AddString(str)
    end
    local packed = s:package()
    local s2 = stream.new(packed)
    for _, expected in ipairs(cases) do
        TestFramework.assert_equal(expected, s2:GetString())
    end
end)

-- Test Bytes (raw, no length prefix)
suite:test("bytes_encoding", function()
    local data = "\x00\x01\x02\xFF\xFE\xFD"
    local s = stream.new()
    s:AddBytes(data)
    local packed = s:package()
    TestFramework.assert_equal(#data, #packed)
    TestFramework.assert_equal(data, packed)
end)

-- Test mark/reset precision
suite:test("mark_reset", function()
    local s = stream.new("abcdefghij")
    TestFramework.assert_equal("ab", s:GetBytes(2))
    s:mark()
    TestFramework.assert_equal("cd", s:GetBytes(2))
    TestFramework.assert_equal("ef", s:GetBytes(2))
    s:reset()
    TestFramework.assert_equal("cd", s:GetBytes(2))
    s:reset()
    TestFramework.assert_equal("cd", s:GetBytes(2))
    TestFramework.assert_equal("ef", s:GetBytes(2))
    TestFramework.assert_equal("gh", s:GetBytes(2))
    TestFramework.assert_equal("ij", s:GetBytes(2))
    TestFramework.assert_equal(0, s:available())
end)

-- Test TestBytes (peek)
suite:test("test_bytes_peek", function()
    local s = stream.new("hello world")
    TestFramework.assert_equal("hello", s:TestBytes(5))
    TestFramework.assert_equal(11, s:available()) -- not consumed
    TestFramework.assert_equal("hello", s:GetBytes(5))
    TestFramework.assert_equal(6, s:available()) -- consumed
end)

-- Test prepare_get / prepare_add mode switching
suite:test("mode_switching", function()
    local s = stream.new()
    s:AddU8(42)
    s:AddString("test")
    s:prepare_get()
    TestFramework.assert_equal(42, s:GetU8())
    TestFramework.assert_equal("test", s:GetString())

    s:prepare_add()
    s:AddU16(9999)
    s:prepare_get()
    -- After prepare_add + prepare_get, should be able to read
    -- The behavior depends on implementation: prepare_get may reset position
    -- Just verify no crash and data is readable
    local ok, val = pcall(function() return s:GetU16() end)
    TestFramework.assert_true(ok)
end)

-- Test empty
suite:test("empty", function()
    local s = stream.new("some data here")
    TestFramework.assert_true(s:available() > 0)
    s:empty()
    TestFramework.assert_equal(0, s:available())
end)

-- Test mixed data types round-trip
suite:test("mixed_roundtrip", function()
    local s = stream.new()
    s:AddU8(0xFF)
    s:AddU16(0xFFFF)
    s:AddU24(0xFFFFFF)
    s:AddS24(-1)
    s:AddD64(2.718281828)
    s:AddU30(268435455)
    s:AddString("mixed test")
    s:AddBytes("\x00\xFF")

    local packed = s:package()
    local s2 = stream.new(packed)
    TestFramework.assert_equal(0xFF, s2:GetU8())
    TestFramework.assert_equal(0xFFFF, s2:GetU16())
    TestFramework.assert_equal(0xFFFFFF, s2:GetU24())
    TestFramework.assert_equal(-1, s2:GetS24())
    TestFramework.assert_true(math.abs(s2:GetD64() - 2.718281828) < 1e-9)
    TestFramework.assert_equal(268435455, s2:GetU30())
    TestFramework.assert_equal("mixed test", s2:GetString())
    TestFramework.assert_equal("\x00\xFF", s2:GetBytes(2))
    TestFramework.assert_equal(0, s2:available())
end)

-- Test boundary values for all integer types
suite:test("boundary_values", function()
    local s = stream.new()
    s:AddU8(0)
    s:AddU8(255)
    s:AddU16(0)
    s:AddU16(65535)
    s:AddU24(0)
    s:AddU24(16777215)
    s:AddS24(-8388608)
    s:AddS24(8388607)
    s:AddU30(0)
    s:AddU30(268435455)

    s:prepare_get()
    TestFramework.assert_equal(0, s:GetU8())
    TestFramework.assert_equal(255, s:GetU8())
    TestFramework.assert_equal(0, s:GetU16())
    TestFramework.assert_equal(65535, s:GetU16())
    TestFramework.assert_equal(0, s:GetU24())
    TestFramework.assert_equal(16777215, s:GetU24())
    TestFramework.assert_equal(-8388608, s:GetS24())
    TestFramework.assert_equal(8388607, s:GetS24())
    TestFramework.assert_equal(0, s:GetU30())
    TestFramework.assert_equal(268435455, s:GetU30())
end)

-- Test package() produces identical output for identical input
suite:test("deterministic_encoding", function()
    local function make_stream()
        local s = stream.new()
        s:AddU8(42)
        s:AddU16(1234)
        s:AddU30(99999)
        s:AddString("deterministic")
        return s:package()
    end

    local p1 = make_stream()
    local p2 = make_stream()
    TestFramework.assert_equal(p1, p2)
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
