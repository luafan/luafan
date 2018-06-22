local core = require "fan.objectbuf.core"
local fan = require "fan"

local CTX_INDEX_TABLES = 1
local CTX_INDEX_NUMBERS = 2
local CTX_INDEX_STRINGS = 3
local CTX_INDEX_FUNCS = 4
local CTX_INDEX_U30S = 5

local SYM_INDEX_MAP = 1
local SYM_INDEX_MAP_VK = 2
local SYM_INDEX_INDEX = 3

local function encode(buf, ...)
    return core.encode(buf, ...)
end

local function decode(buf, ...)
    local obj, msg = core.decode(buf, ...)

    if obj == nil then
        print(msg, fan.data2hex(buf))
    end

    return obj
end

return {
    encode = encode,
    decode = decode,
    sample = function(obj, optional_result_count)
        local count_map = {}
        local count_list = {}
        if type(optional_result_count) ~= "number" then
            optional_result_count = 127
        end

        local function add_count(v)
            local m = count_map[v]
            if not m then
                m = {key = v, count = 1}
                table.insert(count_list, m)
                count_map[v] = m
            else
                m.count = m.count + 1
            end
        end

        local count_table

        local function count_object(v)
            if type(v) == "table" then
                count_table(v)
            else
                add_count(v)
            end
        end

        count_table = function(t)
            local total = 0
            while true do
                local v = t[total + 1]
                if not v then
                    break
                end
                total = total + 1

                count_object(v)
            end

            for k, v in pairs(t) do
                if type(k) ~= "number" or k > total or k <= 0 then
                    count_object(k)
                    count_object(v)
                end
            end
        end

        count_object(obj)

        table.sort(
            count_list,
            function(a, b)
                return a.count > b.count
            end
        )

        local sym_map = {}

        for i, v in ipairs(count_list) do
            if i > optional_result_count then
                break
            end

            table.insert(sym_map, v.key)
        end

        return sym_map
    end,
    symbol = function(obj)
        local ctx = core.symbol(obj)

        local index_map = {}
        local index_map_vk = {}
        local index = 2

        index_map[false] = 1
        index_map[true] = 2

        table.sort(
            ctx[CTX_INDEX_STRINGS],
            function(a, b)
                return a < b
            end
        )

        for i, v in ipairs(ctx[CTX_INDEX_STRINGS]) do
            index_map[v] = index + i
            index_map_vk[index + i] = v
        end

        index = index + #(ctx[CTX_INDEX_STRINGS])

        table.sort(
            ctx[CTX_INDEX_NUMBERS],
            function(a, b)
                return a < b
            end
        )

        for i, v in ipairs(ctx[CTX_INDEX_NUMBERS]) do
            index_map[v] = index + i
            index_map_vk[index + i] = v
        end

        index = index + #(ctx[CTX_INDEX_NUMBERS])

        table.sort(
            ctx[CTX_INDEX_U30S],
            function(a, b)
                return a < b
            end
        )

        for i, v in ipairs(ctx[CTX_INDEX_U30S]) do
            index_map[v] = index + i
            index_map_vk[index + i] = v
        end

        index = index + #(ctx[CTX_INDEX_U30S])

        return {[SYM_INDEX_MAP] = index_map, [SYM_INDEX_MAP_VK] = index_map_vk, [SYM_INDEX_INDEX] = index}
    end
}
