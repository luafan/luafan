local lfs = require "lfs"

local config_dir = "config"
local configd_dir = "config.d"

local env = {os = os, tonumber = tonumber}

local function load_config(dir)
    local attr = lfs.attributes(dir)
    if attr and attr.mode == "directory" then
        for name in lfs.dir(dir) do
            if name:match("^[^.].*[.]lua$") then
                loadfile(string.format("%s/%s", dir, name), "t", env)()
            end
        end
    else
      print(string.format("config [%s] not found!", dir))
    end
end

load_config(config_dir)
load_config(configd_dir)

return env
