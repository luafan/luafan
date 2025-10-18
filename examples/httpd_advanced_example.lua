#!/usr/bin/env lua
-- Advanced HTTPD Example
-- Demonstrates all new features: routing, security, performance monitoring

local fan = require "fan"
local httpd = require "modules.fan.httpd.httpd"

-- Sample data for demonstration
local users = {
    ["1"] = {id = 1, name = "Alice", email = "alice@example.com"},
    ["2"] = {id = 2, name = "Bob", email = "bob@example.com"},
    ["3"] = {id = 3, name = "Charlie", email = "charlie@example.com"}
}

-- Simple JSON encoder (for demo purposes)
local function encode_json(obj)
    if type(obj) == "table" then
        local parts = {}
        for k, v in pairs(obj) do
            if type(v) == "string" then
                table.insert(parts, string.format('"%s":"%s"', k, v))
            elseif type(v) == "number" then
                table.insert(parts, string.format('"%s":%s', k, v))
            end
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return '""'
end

-- Create server with enhanced features
local server = httpd.bind({
    host = "127.0.0.1",
    port = 8080
})

print("🚀 Starting Advanced HTTPD Server...")
print("📡 Server running at http://127.0.0.1:8080")
print("✨ Features enabled: Routing, Security, Performance Monitoring, Gzip")

-- Logging middleware
server:use(function(ctx, next)
    local start_time = os.time()
    print(string.format("📝 [%s] %s %s from %s", os.date("%H:%M:%S"), ctx.method, ctx.path, ctx.remoteip))
    next()
    local duration = os.time() - start_time
    print(string.format("✅ Request completed in %ds", duration))
end)

-- Authentication middleware (demo)
local function auth_middleware(ctx, next)
    local auth_header = ctx.headers["authorization"]
    if ctx.path:find("/api/") and not auth_header then
        ctx:reply(401, "Unauthorized", "API requires authentication")
        return
    end
    next()
end

server:use(auth_middleware)

-- Routes

-- Home page
server:get("/", function(ctx)
    local html = [[
<!DOCTYPE html>
<html>
<head>
    <title>LuaFan Advanced HTTPD Demo</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        .feature { background: #f0f8ff; padding: 10px; margin: 10px 0; border-radius: 5px; }
        .endpoint { background: #f5f5f5; padding: 5px; margin: 5px 0; font-family: monospace; }
    </style>
</head>
<body>
    <h1>🚀 LuaFan Advanced HTTPD Demo</h1>

    <div class="feature">
        <h3>✨ New Features</h3>
        <ul>
            <li>RESTful Routing with Path Parameters</li>
            <li>Middleware Pipeline</li>
            <li>Security Headers & Rate Limiting</li>
            <li>Intelligent Gzip Compression</li>
            <li>HTTP/1.1 Keep-Alive</li>
            <li>Performance Monitoring</li>
        </ul>
    </div>

    <div class="feature">
        <h3>🔗 Available Endpoints</h3>
        <div class="endpoint">GET /</div>
        <div class="endpoint">GET /users</div>
        <div class="endpoint">GET /users/:id</div>
        <div class="endpoint">POST /users</div>
        <div class="endpoint">GET /large</div>
        <div class="endpoint">GET /stats</div>
        <div class="endpoint">GET /api/protected (requires auth)</div>
    </div>

    <div class="feature">
        <h3>🧪 Try These Commands</h3>
        <pre>
curl http://127.0.0.1:8080/users/1
curl http://127.0.0.1:8080/stats
curl -H "Accept-Encoding: gzip" http://127.0.0.1:8080/large
curl -X POST http://127.0.0.1:8080/users -d '{"name":"David"}'
        </pre>
    </div>
</body>
</html>
    ]]
    ctx:addheader("Content-Type", "text/html; charset=utf-8")
    ctx:reply(200, "OK", html)
end)

-- List all users
server:get("/users", function(ctx)
    local user_list = {}
    for id, user in pairs(users) do
        table.insert(user_list, user)
    end

    ctx:addheader("Content-Type", "application/json")
    ctx:reply(200, "OK", encode_json(user_list))
end)

-- Get specific user by ID (demonstrates path parameters)
server:get("/users/:id", function(ctx)
    local user_id = ctx.params.id
    local user = users[user_id]

    if user then
        ctx:addheader("Content-Type", "application/json")
        ctx:reply(200, "OK", encode_json(user))
    else
        ctx:reply(404, "Not Found", "User not found")
    end
end)

-- Create new user (demonstrates POST with body)
server:post("/users", function(ctx)
    local body = ctx.body or ""
    local next_id = tostring(#users + 1)

    -- Simple user creation (in real app, parse JSON properly)
    users[next_id] = {
        id = tonumber(next_id),
        name = "New User " .. next_id,
        email = "user" .. next_id .. "@example.com"
    }

    ctx:addheader("Content-Type", "application/json")
    ctx:reply(201, "Created", encode_json(users[next_id]))
end)

-- Large content for compression testing
server:get("/large", function(ctx)
    local large_content = string.rep("This is a large text document. ", 200) ..
                         string.rep("Lorem ipsum dolor sit amet, consectetur adipiscing elit. ", 100)

    ctx:addheader("Content-Type", "text/plain")
    ctx:reply(200, "OK", large_content)
end)

-- Performance statistics
server:get("/stats", function(ctx)
    local stats = server:get_stats()

    local stats_html = string.format([[
<!DOCTYPE html>
<html>
<head>
    <title>Server Statistics</title>
    <meta http-equiv="refresh" content="5">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        .stat { background: #f0f8ff; padding: 10px; margin: 10px; border-radius: 5px; display: inline-block; }
        .number { font-size: 2em; font-weight: bold; color: #0066cc; }
    </style>
</head>
<body>
    <h1>📊 Server Performance Statistics</h1>
    <p><em>Auto-refreshes every 5 seconds</em></p>

    <div class="stat">
        <div>Uptime</div>
        <div class="number">%ds</div>
    </div>

    <div class="stat">
        <div>Total Requests</div>
        <div class="number">%d</div>
    </div>

    <div class="stat">
        <div>Requests/Second</div>
        <div class="number">%.2f</div>
    </div>

    <div class="stat">
        <div>Error Rate</div>
        <div class="number">%.1f%%</div>
    </div>

    <div class="stat">
        <div>Avg Response Time</div>
        <div class="number">%.2fms</div>
    </div>

    <div class="stat">
        <div>Bytes Sent</div>
        <div class="number">%d</div>
    </div>
</body>
</html>
    ]], stats.uptime_seconds,
        stats.requests_total,
        stats.requests_per_second,
        stats.errors_rate * 100,
        stats.average_response_time_ms,
        stats.bytes_sent_total)

    ctx:addheader("Content-Type", "text/html")
    ctx:reply(200, "OK", stats_html)
end)

-- Protected API endpoint (demonstrates authentication middleware)
server:get("/api/protected", function(ctx)
    ctx:addheader("Content-Type", "application/json")
    ctx:reply(200, "OK", '{"message":"Access granted to protected resource"}')
end)

-- Custom 404 handler
server:set_not_found(function(ctx)
    local html = [[
<!DOCTYPE html>
<html>
<head><title>404 - Not Found</title></head>
<body>
    <h1>🚫 Page Not Found</h1>
    <p>The requested URL was not found on this server.</p>
    <p><a href="/">← Back to Home</a></p>
</body>
</html>
    ]]
    ctx:addheader("Content-Type", "text/html")
    ctx:reply(404, "Not Found", html)
end)

print("\n🌟 Server Features:")
print("   • HTTP/1.1 with Keep-Alive")
print("   • RESTful routing with path parameters")
print("   • Automatic security headers")
print("   • Rate limiting (100 req/min per IP)")
print("   • Intelligent gzip compression")
print("   • Real-time performance monitoring")
print("   • Middleware system")
print("\n🔗 Try: curl http://127.0.0.1:8080/")
print("📊 Stats: curl http://127.0.0.1:8080/stats")

-- Start the event loop
fan.loop()