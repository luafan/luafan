local lfs = require "lfs"

local MODULE_EXT = MODULE_EXT or ".lua"
local MODULE_LOAD_MODE = MODULE_LOAD_MODE or "bt"

local configd_dir = (WORKDIR or "") .. "config.d"

local env = { os = os, tonumber = tonumber, weaktable = {}, WORKDIR = WORKDIR }
setmetatable(env.weaktable, { __mode = "v" })

local function load_config(dir)
    local attr = lfs.attributes(dir)
    if attr and attr.mode == "directory" then
        for name in lfs.dir(dir) do
            if name:sub(1,1) ~= "." and name:sub(-#MODULE_EXT) == MODULE_EXT then
                local path = string.format("%s/%s", dir, name)
                local chunk, err = loadfile(path, MODULE_LOAD_MODE, env)
                if not chunk then
                    print(string.format("[config] load error %s: %s", path, tostring(err)))
                else
                    local ok, ret = pcall(chunk)
                    if not ok then
                        print(string.format("[config] exec error %s: %s", path, tostring(ret)))
                    end
                end
            end
        end
    else
        print(string.format("config [%s] not found, ignored.", dir))
    end
end

load_config(configd_dir)

local t = {}
for k, v in pairs(env) do
    if k ~= "os" and k ~= "tonumber" and k ~= "weaktable" and k ~= "WORKDIR" then
        t[k] = v
    end
end

return t
