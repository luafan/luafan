# fan.popen

Bidirectional subprocess pipe I/O integrated with the luafan event loop.

Uses `posix_spawnp` (no fork) for safe macOS GUI app compatibility.

## Quick Start

```lua
local fan = require "fan"
local popen = require "fan.popen"

fan.loop(function()
    local proc = popen.spawn({
        command = {"echo", "hello world"},
        onread = function(data)
            print("stdout:", data)
        end,
        ondisconnected = function(msg, exit_code)
            print("exit:", exit_code)
            fan.loopbreak()
        end,
    })
end)
```

## C Module: `fan.popen`

```lua
local popen = require "fan.popen"
```

### popen.spawn(opts)

Spawn a child process with bidirectional pipe I/O.

**Parameters** (`opts` table):

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `command` | string or table | required | Command to run. String: `"cat -n"` (split on spaces). Table: `{"cat", "-n"}` (exact argv). |
| `onread` | function | nil | `function(data)` — called when stdout data arrives |
| `onstderr` | function | nil | `function(data)` — called when stderr data arrives |
| `ondisconnected` | function | nil | `function(msg, exit_code)` — called when child exits |
| `capture_stderr` | boolean | true | Whether to capture stderr separately |

**Returns**: `proc` userdata, or `nil, error_message`

### proc:send(data)

Write data to child's stdin.

**Returns**: `bytes_written` (number), or `nil, error_message`

### proc:close_stdin()

Close the stdin pipe without killing the child. The child will see EOF on its stdin.

**Returns**: `true`

### proc:close()

Terminate the child (SIGTERM, then SIGKILL if needed), close all pipes, free events.

**Returns**: `true`. Safe to call multiple times.

### proc:getpid()

**Returns**: child PID (number), or `nil` if already exited.

### proc:is_alive()

**Returns**: `true` if child is still running, `false` otherwise.

## Lua Connector: `fan.connector.popen`

Coroutine-based wrapper around `fan.popen`, providing `send`/`receive` semantics.

```lua
local connector = require "fan.connector.popen"

fan.loop(function()
    local proc = connector.spawn({
        command = {"cat"},
    })

    proc:send("hello\n")

    local data = proc:receive(1)  -- yield until >= 1 byte available
    print("got:", data:available(), "bytes")

    proc:close()
end)
```

### connector.spawn(opts)

Same options as `fan.popen.spawn()`. Returns connector object with:

| Method | Description |
|--------|-------------|
| `proc:send(data)` | Write to stdin |
| `proc:receive(expect)` | Yield until `expect` bytes available, returns stream |
| `proc:close_stdin()` | Close stdin pipe |
| `proc:close()` | Kill child and cleanup |
| `proc:getpid()` | Get child PID |
| `proc:is_alive()` | Check if running |
| `proc.exit_code` | Exit code after disconnect (nil if still running) |

## MCP Client Example

```lua
local fan = require "fan"
local json = require "cjson"
local connector = require "fan.connector.popen"

fan.loop(function()
    local mcp = connector.spawn({
        command = {"xcrun", "mcpbridge"},
    })

    -- Initialize
    mcp:send(json.encode({
        jsonrpc = "2.0",
        id = 1,
        method = "initialize",
        params = {
            protocolVersion = "2024-11-05",
            capabilities = {},
            clientInfo = {name = "luan", version = "1.0"},
        },
    }) .. "\n")

    local resp = mcp:receive(1)
    local init = json.decode(resp)
    print("server:", init.result.serverInfo.name)

    -- Initialized notification
    mcp:send(json.encode({
        jsonrpc = "2.0",
        method = "notifications/initialized",
    }) .. "\n")

    -- List tools
    mcp:send(json.encode({
        jsonrpc = "2.0",
        id = 2,
        method = "tools/list",
    }) .. "\n")

    resp = mcp:receive(1)
    local tools = json.decode(resp)
    for _, tool in ipairs(tools.result.tools) do
        print(" -", tool.name)
    end

    -- Call a tool
    mcp:send(json.encode({
        jsonrpc = "2.0",
        id = 3,
        method = "tools/call",
        params = {
            name = "XcodeListWindows",
            arguments = {},
        },
    }) .. "\n")

    resp = mcp:receive(1)
    local result = json.decode(resp)
    print(json.encode(result))

    mcp:close()
end)
```

## Stdio Protocol Example (JSON-RPC)

```lua
local fan = require "fan"
local popen = require "fan.popen"
local json = require "cjson"

fan.loop(function()
    local proc = popen.spawn({
        command = {"cat"},  -- echo server for testing
        onread = function(data)
            for line in data:gmatch("[^\n]+") do
                local msg = json.decode(line)
                print("recv:", msg.method or msg.id)
            end
        end,
        ondisconnected = function(msg, code)
            print("disconnected:", msg, code)
            fan.loopbreak()
        end,
    })

    -- Send newline-delimited JSON messages
    local function send_json(obj)
        proc:send(json.encode(obj) .. "\n")
    end

    send_json({jsonrpc = "2.0", id = 1, method = "initialize", params = {
        protocolVersion = "2024-11-05",
        capabilities = {},
        clientInfo = {name = "test", version = "0.1"},
    }})

    fan.sleep(1)
    proc:close()
end)
```

## Build Requirements

- luafan C library compiled with `popen.c`
- POSIX system (macOS, Linux)
- libevent >= 2.1
