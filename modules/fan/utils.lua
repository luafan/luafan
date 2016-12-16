local fan = require "fan"
local type = type
local table = table
local math = math

local function random_string(letters, count, join, joingroupcount)
    local tb = {}

    if type(joingroupcount) == "number" then
        for i=1,count/joingroupcount do
            local group = {}
            for i=1,joingroupcount do
                local ri = math.random(1,#(letters))
                table.insert(group, letters:sub(ri, ri))
            end
            table.insert(tb, table.concat(group))
        end
    else
        for i=1,count do
            local ri = math.random(1,#(letters))
            table.insert(tb, letters:sub(ri, ri))
        end
    end

    return table.concat(tb, join)
end

local function gettime()
    local sec,usec = fan.gettime()
    return sec + usec/1000000.0
end

math.randomseed((fan.gettime()))

return {
    random_string = random_string,
    gettime = gettime,
    LETTERS_W = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
}
