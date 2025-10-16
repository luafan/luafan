# fan.utils

Utility functions for common operations including string generation, time handling, weak references, and string manipulation.

## Functions

### random_string(letters, count, join, joingroupcount)

Generate a random string with specified parameters.

**Parameters:**
- `letters` (string): The character set to choose from
- `count` (number): Total number of characters to generate
- `join` (string, optional): String to join groups with
- `joingroupcount` (number, optional): Number of characters per group

**Returns:**
- `string`: Generated random string

**Example:**
```lua
local utils = require "fan.utils"

-- Simple random string
local str1 = utils.random_string(utils.LETTERS_W, 10)
-- Output: "aB3xK9mL2p"

-- Grouped random string
local str2 = utils.random_string(utils.LETTERS_W, 12, "-", 4)
-- Output: "aB3x-K9mL-2p5Q"
```

### gettime()

Get high-precision current time as a floating-point number.

**Returns:**
- `number`: Current time in seconds with microsecond precision

**Example:**
```lua
local utils = require "fan.utils"

local start_time = utils.gettime()
-- ... do some work ...
local elapsed = utils.gettime() - start_time
print("Elapsed time:", elapsed, "seconds")
```

**Note:** On LuaJIT, this function uses FFI for better performance via direct `gettimeofday()` call.

### split(str, pat)

Split a string into an array using a pattern delimiter.

**Parameters:**
- `str` (string): The string to split
- `pat` (string): The pattern to split on (Lua pattern syntax)

**Returns:**
- `table`: Array of string parts

**Example:**
```lua
local utils = require "fan.utils"

local parts = utils.split("hello,world,lua", ",")
-- Result: {"hello", "world", "lua"}

local paths = utils.split("/usr/local/bin", "/")
-- Result: {"usr", "local", "bin"}
```

### weakify(...)

Create weak references to objects to prevent strong reference cycles.

**Parameters:**
- `...` (any): One or more objects to create weak references for

**Returns:**
- Weak reference proxy objects

**Example:**
```lua
local utils = require "fan.utils"

local obj = {name = "test", data = {1, 2, 3}}
local weak_obj = utils.weakify(obj)

-- Access through weak reference
print(weak_obj.name)  -- "test"
weak_obj.new_field = "added"

-- Multiple objects
local obj1, obj2 = {a = 1}, {b = 2}
local weak1, weak2 = utils.weakify(obj1, obj2)
```

### weakify_object(target)

Create a weak reference to a single object.

**Parameters:**
- `target` (any): The object to create a weak reference for

**Returns:**
- Weak reference proxy object

**Example:**
```lua
local utils = require "fan.utils"

local original = {name = "example"}
local weak_ref = utils.weakify_object(original)

print(weak_ref.name)  -- "example"
-- If original goes out of scope, weak_ref operations will return nil
```

## Constants

### LETTERS_W

Pre-defined character set containing alphanumeric characters (both cases) and digits.

**Value:** `"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"`

**Example:**
```lua
local utils = require "fan.utils"

-- Generate random alphanumeric string
local token = utils.random_string(utils.LETTERS_W, 16)
print(token)  -- e.g., "aB3xK9mL2p5QwRtY"
```

## Implementation Notes

- The `gettime()` function automatically uses FFI-optimized implementation when running under LuaJIT
- Weak references help prevent memory leaks in callback-heavy or event-driven code
- The `split()` function uses Lua patterns, not regular expressions
- Random string generation uses `math.random()`, so seed appropriately for cryptographic use cases

## Use Cases

- **Time measurement**: Use `gettime()` for high-precision timing and benchmarking
- **Token generation**: Use `random_string()` for session IDs, temporary tokens, etc.
- **Memory management**: Use `weakify()` to break reference cycles in complex object graphs
- **String processing**: Use `split()` for parsing configuration files, CSV data, etc.