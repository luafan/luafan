#!/usr/bin/env lua
-- MCP (Model Context Protocol) client over stdio using fan.popen
-- Demonstrates communicating with xcrun mcpbridge or any MCP server
-- Run: lua examples/popen_mcp_client.lua

package.preload['config'] = function() return { debug = false } end

local fan = require "fan"
local popen = require "fan.popen"
_G.fan = fan

local json = require "cjson"

local MCPClient = {}
MCPClient.__index = MCPClient

function MCPClient.new(command)
    local self = setmetatable({}, MCPClient)
    self._command = command
    self._proc = nil
    self._buf = ""
    self._id = 0
    self._pending = {}  -- id -> coroutine
    self._notifications = {}
    self._tools = {}
    self._initialized = false
    return self
end

function MCPClient:start()
    local err
    self._proc, err = popen.spawn({
        command = self._command,
        onread = function(data)
            self:_on_data(data)
        end,
        onstderr = function(data)
            io.stderr:write("[mcp stderr] " .. data)
        end,
        ondisconnected = function(msg, code)
            self:_on_disconnected(msg, code)
        end,
    })
    if not self._proc then
        return nil, err
    end
    return self
end

function MCPClient:_on_data(data)
    self._buf = self._buf .. data

    -- Process complete newline-delimited messages
    while true do
        local pos = self._buf:find("\n")
        if not pos then break end

        local line = self._buf:sub(1, pos - 1)
        self._buf = self._buf:sub(pos + 1)

        if #line > 0 then
            local ok, msg = pcall(json.decode, line)
            if ok then
                self:_dispatch(msg)
            end
        end
    end
end

function MCPClient:_dispatch(msg)
    if msg.id and self._pending[msg.id] then
        -- Response to a request
        local co = self._pending[msg.id]
        self._pending[msg.id] = nil
        coroutine.resume(co, msg)
    elseif msg.method then
        -- Server notification
        table.insert(self._notifications, msg)
    end
end

function MCPClient:_on_disconnected(msg, code)
    -- Resume any pending coroutines with nil
    for id, co in pairs(self._pending) do
        coroutine.resume(co, nil)
    end
    self._pending = {}
end

function MCPClient:_send_request(method, params)
    self._id = self._id + 1
    local id = self._id

    local request = {
        jsonrpc = "2.0",
        id = id,
        method = method,
    }
    if params then
        request.params = params
    end

    self._proc:send(json.encode(request) .. "\n")

    -- Yield until response arrives
    local co = coroutine.running()
    self._pending[id] = co
    local response = coroutine.yield()
    self._pending[id] = nil

    return response
end

function MCPClient:_send_notification(method, params)
    local notification = {
        jsonrpc = "2.0",
        method = method,
    }
    if params then
        notification.params = params
    end
    self._proc:send(json.encode(notification) .. "\n")
end

function MCPClient:initialize(client_info)
    local resp = self:_send_request("initialize", {
        protocolVersion = "2024-11-05",
        capabilities = {},
        clientInfo = client_info or {name = "luan-mcp-client", version = "1.0"},
    })

    if not resp then
        return nil, "no response from server"
    end

    self:_send_notification("notifications/initialized")
    self._initialized = true
    return resp.result
end

function MCPClient:list_tools()
    local resp = self:_send_request("tools/list")
    if not resp or not resp.result then
        return nil, "failed to list tools"
    end
    self._tools = resp.result.tools or {}
    return self._tools
end

function MCPClient:call_tool(name, arguments)
    local resp = self:_send_request("tools/call", {
        name = name,
        arguments = arguments or {},
    })
    return resp
end

function MCPClient:close()
    if self._proc then
        self._proc:close()
        self._proc = nil
    end
end

-- ============================================================
-- Example usage with echo/cat (simulated MCP server)
-- ============================================================
local function run_example()
    fan.loop(function()
        print("Starting MCP client example...\n")

        -- For real usage: local client = MCPClient.new({"xcrun", "mcpbridge"})
        -- For demo: use cat as echo server
        local client = MCPClient.new({"cat"})
        local ok, err = client:start()
        if not ok then
            print("Failed to start:", err)
            fan.loopbreak()
            return
        end

        -- Initialize (cat echoes back our request, which becomes our "response")
        -- In a real MCP server, the response would come from the server
        print("--- Sending initialize ---")
        client._proc:send(json.encode({
            jsonrpc = "2.0", id = 1, method = "initialize",
            params = {
                protocolVersion = "2024-11-05",
                capabilities = {},
                clientInfo = {name = "test", version = "0.1"},
            },
        }) .. "\n")

        fan.sleep(0.5)
        print("Buffer so far:", client._buf:sub(1, 200))

        -- Send initialized notification
        client:_send_notification("notifications/initialized")
        print("Sent notifications/initialized")

        -- List tools
        print("\n--- Sending tools/list ---")
        client._proc:send(json.encode({
            jsonrpc = "2.0", id = 2, method = "tools/list",
        }) .. "\n")

        fan.sleep(0.5)

        -- Close
        client:close()
        print("\nClient closed.")
        fan.loopbreak()
    end)
end

-- Check if cjson is available
local has_cjson, _ = pcall(require, "cjson")
if not has_cjson then
    print("Note: cjson not available, skipping MCP client example")
    print("Install with: luarocks install lua-cjson")
    os.exit(0)
end

run_example()
