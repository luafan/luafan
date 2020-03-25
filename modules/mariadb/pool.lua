local setmetatable = setmetatable
local getmetatable = getmetatable
local pairs = pairs
local pcall = pcall
local table = table
local coroutine = coroutine

local mariadb = require "fan.mariadb"

local orm = require "mariadb.orm"
local config = require "config"

local pool_mt = {}
pool_mt.__index = pool_mt

local function maxn(t)
    local n = 0
    for k, _ in pairs(t) do
        if k > n then
            n = k
        end
    end

    return n
end

function pool_mt:pop()
    local conn

    if #(self.idle) > 0 then
        conn = table.remove(self.idle)
    elseif self.count > 0 then
        self.count = self.count - 1
        conn =
            assert(
            mariadb.connect(
                config.maria_database,
                config.maria_user,
                config.maria_passwd,
                config.maria_host,
                config.maria_port
            )
        )
        assert(conn:setcharset(config.maria_charset or "utf8mb4"))
    else
        if not self.yielding.head then
            self.yielding.head = {value = coroutine.running()}
            self.yielding.tail = self.yielding.head
        else
            self.yielding.tail.next = {value = coroutine.running()}
            self.yielding.tail = self.yielding.tail.next
        end
        conn = coroutine.yield()
    end

    local ctx = self.map[conn]
    if not ctx then
        local index = self.index
        self.index = self.index + 1
        ctx = orm.new(conn, table.unpack(self.args, 1, maxn(self.args)))
        self.map[conn] = ctx
        ctx.index = index
    end

    return ctx
end

function pool_mt:push(ctx)
    local conn = getmetatable(ctx).db
    if self.yielding.head then
        local co = self.yielding.head.value
        self.yielding.head = self.yielding.head.next
        local status, msg = coroutine.resume(co, conn)
        if not status then
            print(msg)
        end
    else
        table.insert(self.idle, conn)
    end
end

function pool_mt:safe(func, ...)
    local ctx = self:pop()
    local st, msg = xpcall(func, debug.traceback, ctx, ...)
    self:push(ctx)

    if not st then
        print(msg)
    else
        return msg
    end
end

local function new(...)
    local args = {...}
    local obj = {
        args = args,
        map = {},
        idle = {},
        yielding = {head = nil, tail = nil},
        count = config.maria_pool_size or 10,
        index = 0,
    }
    setmetatable(obj, pool_mt)

    return obj
end

return {
    new = new
}
