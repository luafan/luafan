local setmetatable = setmetatable
local getmetatable = getmetatable
local pairs = pairs
local pcall = pcall
local table = table
local ipairs = ipairs
local coroutine = coroutine

local mariadb = require "fan.mariadb"

local orm = require "mariadb.orm"
local config = require "config"

local pool_mt = {}
pool_mt.__index = pool_mt

local pool = {idle = {}, yielding = {head = nil, tail = nil}, count = config.maria_pool_size}

local function maxn(t)
    local n = 0
    for k, v in pairs(t) do
        if k > n then
            n = k
        end
    end

    return n
end

function pool_mt:pop()
    local conn = nil

    while not conn do
        if #(pool.idle) > 0 then
            conn = table.remove(pool.idle)
            break
        elseif pool.count > 0 then
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
            assert(conn:setcharset(config.maria_charset))
            pool.count = pool.count - 1
            break
        else
            if not pool.yielding.head then
                pool.yielding.head = {value = coroutine.running()}
                pool.yielding.tail = pool.yielding.head
            else
                pool.yielding.tail.next = {value = coroutine.running()}
                pool.yielding.tail = pool.yielding.tail.next
            end
            conn = coroutine.yield()
            break
        end
    end

    local ctx = self.map[conn]
    if not ctx then
        ctx = orm.new(conn, table.unpack(self.args, 1, maxn(self.args)))
        self.map[conn] = ctx
    end

    return ctx
end

function pool_mt:push(ctx)
    local conn = getmetatable(ctx).db
    if pool.yielding.head then
        local co = pool.yielding.head.value
        pool.yielding.head = pool.yielding.head.next
        local status, msg = coroutine.resume(co, conn)
        if not status then
            print(msg)
        end
    else
        table.insert(pool.idle, conn)
    end
end

local function _safe(self, func, ...)
    local ctx = self:pop()
    local st, msg = pcall(func, ctx, ...)
    self:push(ctx)

    if not st then
        error(msg)
    else
        return msg
    end
end

function pool_mt:safe(func, ...)
    local st, msg = pcall(_safe, self, func, ...)

    if not st then
        error(msg)
    else
        return msg
    end
end

local function new(...)
    local args = {...}
    local obj = {args = args, map = {}}
    setmetatable(obj, pool_mt)

    return obj
end

return {
    new = new
}
