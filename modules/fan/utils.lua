local fan = require "fan"
local type = type
local table = table
local math = math
local tonumber = tonumber
local setmetatable = setmetatable

local function random_string(letters, count, join, joingroupcount)
    local tb = {}

    if type(joingroupcount) == "number" then
        for _ = 1, count / joingroupcount do
            local group = {}
            for _ = 1, joingroupcount do
                local ri = math.random(1, #(letters))
                table.insert(group, letters:sub(ri, ri))
            end
            table.insert(tb, table.concat(group))
        end
    else
        for _ = 1, count do
            local ri = math.random(1, #(letters))
            table.insert(tb, letters:sub(ri, ri))
        end
    end

    return table.concat(tb, join)
end

local function gettime()
    local sec, usec = fan.gettime()
    return sec + usec / 1000000.0
end

local weak_mt = {}
weak_mt.__index = function(self, key)
    local target = self[weak_mt].target
    if target then
        return target[key]
    end
end

weak_mt.__newindex = function(self, key, value)
    local target = self[weak_mt].target
    if target then
        target[key] = value
    end
end

local function weakify_object(target)
    local obj = {[weak_mt] = setmetatable({target = target}, {__mode = "v"})}
    setmetatable(obj, weak_mt)
    return obj
end

local function weakify(...)
    local t = {...}
    if #t > 1 then
        local out = {}
        for _, v in ipairs(t) do
            table.insert(out, weakify_object(v))
        end

        return table.unpack(out)
    else
        return weakify_object(t[1])
    end
end

local function split(str, pat)
    local t = {}
    if str then
        local fpat = "(.-)" .. pat
        local last_end = 1
        local s, e, cap = str:find(fpat, 1)
        while s do
            if s ~= 1 or cap ~= "" then
                table.insert(t, cap)
            end
            last_end = e + 1
            s, e, cap = str:find(fpat, last_end)
        end
        if last_end <= #str then
            cap = str:sub(last_end)
            table.insert(t, cap)
        end
    end
    return t
end

local m = {
    random_string = random_string,
    gettime = gettime,
    split = split,
    LETTERS_W = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
    weakify = weakify,
    weakify_object = weakify_object
}

if _G.jit then
    local ffi = require("ffi")

    ffi.cdef [[
    typedef long time_t;
    typedef struct timeval {
      time_t tv_sec;
      time_t tv_usec;
    } timeval;

    int gettimeofday(struct timeval* t, void* tzp);
  ]]

    local t = ffi.new("timeval")
    function m.gettime()
        ffi.C.gettimeofday(t, nil)
        return tonumber(t.tv_sec) + tonumber(t.tv_usec) / 1000000.0
    end
end

return m
