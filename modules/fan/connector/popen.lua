local fan = require "fan"
local popen = require "fan.popen"
local stream = require "fan.stream"

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
    if self.disconnected then return nil, "disconnected" end
    return self._proc:send(buf)
end

function apt_mt:close_stdin()
    if self._proc then
        self._proc:close_stdin()
    end
end

function apt_mt:receive(expect)
    if self.disconnected then return nil, "disconnected" end

    expect = expect or 1

    if self._readstream:available() >= expect then
        return self._readstream
    else
        self.receiving_expect = expect
        self.receiving = coroutine.running()
        return coroutine.yield()
    end
end

function apt_mt:_onread(data)
    self._readstream:prepare_add()
    self._readstream:AddBytes(data)
    self._readstream:prepare_get()

    if self.receiving and self._readstream:available() >= self.receiving_expect then
        local receiving = self.receiving
        self.receiving = nil
        self.receiving_expect = 0
        local st, msg = coroutine.resume(receiving, self._readstream)
        if not st then print(msg) end
    end
end

function apt_mt:_onstderr(data)
    if self.onstderr then
        self.onstderr(data)
    end
end

function apt_mt:_ondisconnected(msg, exit_code)
    self.disconnected = true
    self.exit_code = exit_code

    if self.receiving then
        local receiving = self.receiving
        self.receiving = nil
        coroutine.resume(receiving, nil)
    end

    if self.ondisconnected then
        self.ondisconnected(msg, exit_code)
    end
end

function apt_mt:close()
    if self._proc then
        self._proc:close()
        self._proc = nil
    end
    self.disconnected = true
end

function apt_mt:getpid()
    if self._proc then
        return self._proc:getpid()
    end
    return nil
end

function apt_mt:is_alive()
    if self._proc then
        return self._proc:is_alive()
    end
    return false
end

-- Spawn a subprocess with coroutine-based I/O
-- opts: same as fan.popen.spawn(), plus optional onstderr/ondisconnected
local function spawn(opts)
    local obj = {
        _readstream = stream.new(),
        _proc = nil,
        disconnected = false,
        receiving = nil,
        receiving_expect = 0,
        exit_code = nil,
        onstderr = opts.onstderr,
        ondisconnected = opts.ondisconnected,
    }
    setmetatable(obj, apt_mt)

    local popen_opts = {
        command = opts.command,
        capture_stderr = opts.capture_stderr,
        onread = function(data)
            obj:_onread(data)
        end,
        onstderr = opts.onstderr and function(data)
            obj:_onstderr(data)
        end or nil,
        ondisconnected = function(msg, exit_code)
            obj:_ondisconnected(msg, exit_code)
        end,
    }

    local proc, err = popen.spawn(popen_opts)
    if not proc then
        return nil, err
    end

    obj._proc = proc
    return obj
end

return {
    spawn = spawn,
}
