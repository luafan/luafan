#!/usr/bin/env lua

-- Test for fan.objectbuf module (modules/fan/objectbuf/)

local TestFramework = require('test_framework')
local fan = require "fan"
local objectbuf = require "fan.objectbuf"

-- Create test suite
local suite = TestFramework.create_suite("fan.objectbuf Tests")

print("Testing fan.objectbuf module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(objectbuf)
    TestFramework.assert_type(objectbuf, "table")

    -- Check for essential functions
    TestFramework.assert_type(objectbuf.encode, "function")
    TestFramework.assert_type(objectbuf.decode, "function")
    TestFramework.assert_type(objectbuf.symbol, "function")
    TestFramework.assert_type(objectbuf.sample, "function")
end)

-- Test basic encode/decode operations
suite:test("basic_encode_decode", function()
    -- Test simple table
    local original = {name = "test", value = 42}
    local encoded = objectbuf.encode(original)
    TestFramework.assert_type(encoded, "string")
    TestFramework.assert_true(#encoded > 0)

    local decoded = objectbuf.decode(encoded)
    TestFramework.assert_type(decoded, "table")
    TestFramework.assert_equal(original.name, decoded.name)
    TestFramework.assert_equal(original.value, decoded.value)
end)

-- Test different data types
suite:test("data_types", function()
    -- Test numbers
    local num_data = {int = 123, float = 3.14}
    local encoded = objectbuf.encode(num_data)
    local decoded = objectbuf.decode(encoded)
    TestFramework.assert_equal(num_data.int, decoded.int)
    TestFramework.assert_equal(num_data.float, decoded.float)

    -- Test strings
    local str_data = {short = "hi", long = "This is a longer string for testing"}
    local encoded2 = objectbuf.encode(str_data)
    local decoded2 = objectbuf.decode(encoded2)
    TestFramework.assert_equal(str_data.short, decoded2.short)
    TestFramework.assert_equal(str_data.long, decoded2.long)

    -- Test booleans
    local bool_data = {true_val = true, false_val = false}
    local encoded3 = objectbuf.encode(bool_data)
    local decoded3 = objectbuf.decode(encoded3)
    TestFramework.assert_equal(bool_data.true_val, decoded3.true_val)
    TestFramework.assert_equal(bool_data.false_val, decoded3.false_val)
end)

-- Test arrays
suite:test("arrays", function()
    -- Test simple array
    local array_data = {items = {1, 2, 3, 4, 5}}
    local encoded = objectbuf.encode(array_data)
    local decoded = objectbuf.decode(encoded)

    TestFramework.assert_type(decoded.items, "table")
    TestFramework.assert_equal(#array_data.items, #decoded.items)
    for i = 1, #array_data.items do
        TestFramework.assert_equal(array_data.items[i], decoded.items[i])
    end

    -- Test mixed type array
    local mixed_array = {data = {"hello", 42, true, nil, "world"}}
    local encoded2 = objectbuf.encode(mixed_array)
    local decoded2 = objectbuf.decode(encoded2)

    TestFramework.assert_equal(mixed_array.data[1], decoded2.data[1])
    TestFramework.assert_equal(mixed_array.data[2], decoded2.data[2])
    TestFramework.assert_equal(mixed_array.data[3], decoded2.data[3])
    TestFramework.assert_equal(mixed_array.data[5], decoded2.data[5])
end)

-- Test nested structures
suite:test("nested_structures", function()
    local nested_data = {
        user = {
            name = "John",
            age = 30,
            address = {
                street = "123 Main St",
                city = "Anytown",
                coordinates = {lat = 40.7128, lng = -74.0060}
            }
        },
        items = {
            {id = 1, name = "Item 1"},
            {id = 2, name = "Item 2"}
        }
    }

    local encoded = objectbuf.encode(nested_data)
    local decoded = objectbuf.decode(encoded)

    -- Check user data
    TestFramework.assert_equal(nested_data.user.name, decoded.user.name)
    TestFramework.assert_equal(nested_data.user.age, decoded.user.age)
    TestFramework.assert_equal(nested_data.user.address.street, decoded.user.address.street)
    TestFramework.assert_equal(nested_data.user.address.city, decoded.user.address.city)
    TestFramework.assert_equal(nested_data.user.address.coordinates.lat, decoded.user.address.coordinates.lat)
    TestFramework.assert_equal(nested_data.user.address.coordinates.lng, decoded.user.address.coordinates.lng)

    -- Check items array
    TestFramework.assert_equal(#nested_data.items, #decoded.items)
    for i = 1, #nested_data.items do
        TestFramework.assert_equal(nested_data.items[i].id, decoded.items[i].id)
        TestFramework.assert_equal(nested_data.items[i].name, decoded.items[i].name)
    end
end)

-- Test empty and nil values
suite:test("empty_and_nil", function()
    -- Test empty table
    local empty_data = {}
    local encoded = objectbuf.encode(empty_data)
    local decoded = objectbuf.decode(encoded)
    TestFramework.assert_type(decoded, "table")

    -- Test table with nil values
    local nil_data = {a = 1, b = nil, c = 3}
    local encoded2 = objectbuf.encode(nil_data)
    local decoded2 = objectbuf.decode(encoded2)
    TestFramework.assert_equal(nil_data.a, decoded2.a)
    TestFramework.assert_equal(nil_data.c, decoded2.c)
    -- nil values typically get omitted in serialization
end)

-- Test symbols for optimization (based on docs)
suite:test("symbols", function()
    if objectbuf.symbol then
        -- Create test data with repeated patterns (good for symbol optimization)
        local test_data = {
            users = {}
        }

        for i = 1, 10 do
            test_data.users[i] = {
                name = "User " .. i,
                email = "user" .. i .. "@example.com",
                status = "active", -- repeated string
                type = "premium"   -- repeated string
            }
        end

        -- Create symbol table from sample data
        local sym = objectbuf.symbol(test_data)
        TestFramework.assert_not_nil(sym)

        -- Test encoding with and without symbols
        local encoded_normal = objectbuf.encode(test_data)
        local encoded_with_sym = objectbuf.encode(test_data, sym)

        TestFramework.assert_type(encoded_normal, "string")
        TestFramework.assert_type(encoded_with_sym, "string")

        -- Symbol version should typically be smaller for data with repeated patterns
        TestFramework.assert_true(#encoded_with_sym <= #encoded_normal)

        -- Test decoding with symbols
        local decoded_normal = objectbuf.decode(encoded_normal)
        local decoded_with_sym = objectbuf.decode(encoded_with_sym, sym)

        -- Both should produce equivalent results
        TestFramework.assert_equal(#test_data.users, #decoded_normal.users)
        TestFramework.assert_equal(#test_data.users, #decoded_with_sym.users)
        TestFramework.assert_equal(test_data.users[1].name, decoded_normal.users[1].name)
        TestFramework.assert_equal(test_data.users[1].name, decoded_with_sym.users[1].name)
    end
end)

-- Test large data performance (inspired by docs benchmark)
suite:test("large_data", function()
    -- Create a complex data structure similar to docs benchmark
    local large_data = {
        b = {
            1234556789,
            12345.6789,
            nil,
            -1234556789,
            -12345.6789,
            0,
            "asdfa",
            d = {
                e = "nested value"
            }
        },
        averyvery = "long long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long text",
    }

    -- Add some random strings like in the benchmark
    math.randomseed(12345) -- Fixed seed for reproducible tests
    for i = 1, 50 do -- Reduced from 100 to keep test fast
        table.insert(large_data.b, string.rep("abc", math.random(1, 100)))
    end

    -- Test normal encoding
    local start_time = os.clock()
    local encoded = objectbuf.encode(large_data)
    local encode_time = os.clock() - start_time

    TestFramework.assert_type(encoded, "string")
    TestFramework.assert_true(#encoded > 1000) -- Should be reasonably large

    start_time = os.clock()
    local decoded = objectbuf.decode(encoded)
    local decode_time = os.clock() - start_time

    TestFramework.assert_equal(large_data.averyvery, decoded.averyvery)
    TestFramework.assert_equal(large_data.b[1], decoded.b[1])
    TestFramework.assert_equal(large_data.b.d.e, decoded.b.d.e)

    -- Test with symbol optimization
    local sym = objectbuf.symbol(large_data)
    start_time = os.clock()
    local encoded_sym = objectbuf.encode(large_data, sym)
    local encode_sym_time = os.clock() - start_time

    start_time = os.clock()
    local decoded_sym = objectbuf.decode(encoded_sym, sym)
    local decode_sym_time = os.clock() - start_time

    -- Symbol version should be smaller for data with repeated patterns
    TestFramework.assert_true(#encoded_sym <= #encoded)
    TestFramework.assert_equal(large_data.averyvery, decoded_sym.averyvery)

    -- Performance should be reasonable (less than 100ms for this test)
    TestFramework.assert_true(encode_time < 0.1)
    TestFramework.assert_true(decode_time < 0.1)
    TestFramework.assert_true(encode_sym_time < 0.1)
    TestFramework.assert_true(decode_sym_time < 0.1)
end)

-- Test round-trip integrity
suite:test("round_trip_integrity", function()
    local test_cases = {
        -- Simple types
        42,
        "hello world",
        true,
        false,

        -- Complex structures
        {a = 1, b = "test", c = {nested = true}},
        {1, 2, 3, "four", {five = 5}},

        -- Edge cases
        {[""] = "empty key", [1.5] = "float key"},
    }

    for i, original in ipairs(test_cases) do
        local encoded = objectbuf.encode(original)
        local decoded = objectbuf.decode(encoded)

        -- For simple types, direct comparison
        if type(original) ~= "table" then
            TestFramework.assert_equal(original, decoded)
        else
            -- For tables, check structure (simplified check)
            TestFramework.assert_type(decoded, "table")
        end
    end
end)

-- Test error handling and edge cases
suite:test("error_handling", function()
    -- objectbuf.decode is designed to be robust and handle malformed data gracefully

    -- Test decoding invalid data - should not crash
    local result1 = objectbuf.decode("invalid binary data")
    -- Implementation may return nil or some parsed result - just ensure no crash
    -- No assertion on result as behavior may vary

    -- Test decoding empty string
    local result2 = objectbuf.decode("")
    -- Should handle gracefully without crashing

    -- Test encoding circular references (based on docs: objectbuf supports complex nesting)
    local circular = {}
    circular.self = circular
    local ok3, result3 = pcall(objectbuf.encode, circular)
    -- According to docs, objectbuf supports "complex nesting table that with children reference it self"
    -- So this should succeed
    TestFramework.assert_true(ok3)
    if ok3 then
        TestFramework.assert_type(result3, "string")
        TestFramework.assert_true(#result3 > 0)

        -- Test decoding the circular reference
        local ok4, decoded_circular = pcall(objectbuf.decode, result3)
        TestFramework.assert_true(ok4)
        if ok4 then
            TestFramework.assert_type(decoded_circular, "table")
            -- The self-reference should be preserved
            TestFramework.assert_equal(decoded_circular, decoded_circular.self)
        end
    end
end)

-- Test encoding efficiency
suite:test("encoding_efficiency", function()
    -- Test that encoded size is reasonable for different data types
    local small_string = objectbuf.encode("hi")
    local large_string = objectbuf.encode(string.rep("a", 1000))

    TestFramework.assert_true(#small_string < 10)
    TestFramework.assert_true(#large_string > 1000)
    TestFramework.assert_true(#large_string < 1100) -- Should have reasonable overhead

    -- Test number encoding
    local small_num = objectbuf.encode(42)
    local large_num = objectbuf.encode(123456789)

    TestFramework.assert_true(#small_num < 10)
    TestFramework.assert_true(#large_num < 20)
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)