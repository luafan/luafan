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

if _CONFIG_D_REGISTRY then
    -- 从 amalgamated bundle 加载 config.d 模块
    for name, src in pairs(_CONFIG_D_REGISTRY) do
        local chunk, err = load(src, "@config.d/" .. name, "t", env)
        if not chunk then
            print(string.format("[config] bundle load error %s: %s", name, tostring(err)))
        else
            local ok, ret = pcall(chunk)
            if not ok then
                print(string.format("[config] bundle exec error %s: %s", name, tostring(ret)))
            end
        end
    end
else
    load_config(configd_dir)
end

local t = {}
for k, v in pairs(env) do
    if k ~= "os" and k ~= "tonumber" and k ~= "weaktable" and k ~= "WORKDIR" then
        t[k] = v
    end
end

return t
