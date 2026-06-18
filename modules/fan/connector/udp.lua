local fan = require "fan"
local udpd = require "fan.udpd"
local utils = require "fan.utils"
local config = require "config"
local reliable_udp = require "fan.reliable_udp"

local string = string
local tostring = tostring
local pairs = pairs
local table = table

local gettime = utils.gettime

config.udp_send_total = 0
config.udp_receive_total = 0
config.udp_resend_total = 0

local window_ctrl_head_set = reliable_udp.window_ctrl_head_set
local WAITING_COUNT = reliable_udp.WAITING_COUNT
local CHECK_TIMEOUT_DURATION = reliable_udp.CHECK_TIMEOUT_DURATION
local session_cache = reliable_udp.session_cache

local function connect(host, port, path)
    port = tonumber(port)

    local dest = udpd.make_dest(host, port)
    host = dest:getHost()
    port = dest:getPort()
    local session_cache_key = string.format("%s:%d", host, port)

    local t = session_cache[session_cache_key]
    if not t then
        t = {
            host = host,
            port = port,
            dest = dest
        }

        reliable_udp.init_conn(t)
        session_cache[session_cache_key] = t
    end
    t._suspended = false

    local weak_apt = utils.weakify(t)

    t.conn =
        udpd.new {
        host = host,
        port = port,
        callback_self_first = true,
        onread = function(conn, buf, from)
            config.udp_receive_total = config.udp_receive_total + 1
            weak_apt._WAITING_COUNT = WAITING_COUNT

            if from:getHost() == host and from:getPort() == port then
                weak_apt:_onread(buf)
            end
        end,
        onsendready = function(conn)
            weak_apt._pending_for_send = nil

            if not weak_apt._window_sent then
                weak_apt._window_sent = true
                weak_apt:_send(window_ctrl_head_set .. string.pack("<I4", weak_apt._send_window), "wind")
                return
            end

            local package = weak_apt._output_chain:pop()
            if package then
                local apt = package.apt
                apt.output_chain_count = apt.output_chain_count - 1
                apt:_send_package(package)
            end
        end
    }

    t.stop = false

    coroutine.wrap(
        function()
            while not t.stop do
                t:_check_timeout()
                fan.sleep(CHECK_TIMEOUT_DURATION)
            end
        end
    )()

    return t
end

local function bind(host, port, path)
    local obj = {clientmap = {}}
    obj._main_output_chain = reliable_udp.new_chain()

    local weak_obj = utils.weakify(obj)

    obj.getapt = function(host, port, from, client_key)
        local obj = weak_obj
        local apt = obj.clientmap[client_key]
        if not apt then
            if not from then
                from = udpd.make_dest(host, port)
            end

            host = from:getHost()
            port = from:getPort()

            local session_cache_key = string.format("%s:%d", host, port)

            apt = session_cache[session_cache_key]

            if not apt then
                apt = {
                    host = host,
                    port = port,
                    dest = from,
                    conn = obj.serv,
                    _parent = obj,
                    _client_key = client_key
                }

                reliable_udp.init_conn(apt)
                session_cache[session_cache_key] = apt
            else
                apt.reuse = apt.reuse + 1

                for i, package in ipairs(apt._suspend_list) do
                    obj._main_output_chain:push(package)
                end

                apt._suspend_list = {}
            end

            apt._suspended = false
            apt.last_outgoing_time = 0
            apt.udp_incoming_time = gettime()
            apt.last_incoming_time = gettime()

            obj.clientmap[client_key] = apt

            if obj.onaccept then
                coroutine.wrap(obj.onaccept)(apt)
            end
        elseif not apt.dest then
            apt.dest = from
        end
        return apt
    end
    obj.serv =
        udpd.new {
        interface = config.manual_interface and config.interface or nil,
        bind_port = port,
        callback_self_first = true,
        onsendready = function(conn)
            local obj = weak_obj
            obj._pending_for_send = nil

            for key, apt in pairs(obj.clientmap) do
                if not apt._window_sent then
                    apt:_send(window_ctrl_head_set .. string.pack("<I4", apt._send_window), "wind")
                    apt._window_sent = true
                    return
                end
            end

            while true do
                local package = obj._main_output_chain:pop()
                if package then
                    local apt = package.apt
                    if not apt._suspended then
                        apt.output_chain_count = apt.output_chain_count - 1

                        apt:_send_package(package)
                        break
                    else
                        if config.debug then
                            print("archive package with disconnected client.")
                        end
                        table.insert(apt._suspend_list, package)
                    end
                else
                    break
                end
            end
        end,
        onread = function(conn, buf, from)
            local obj = weak_obj
            config.udp_receive_total = config.udp_receive_total + 1
            local apt = obj.getapt(nil, nil, from, tostring(from))
            apt.udp_incoming_time = gettime()
            apt.udp_receive_total = apt.udp_receive_total + 1
            apt._WAITING_COUNT = WAITING_COUNT

            apt:_onread(buf)
        end
    }

    obj.stop = false

    coroutine.wrap(
        function()
            while not obj.stop do
                for clientkey, apt in pairs(obj.clientmap) do
                    if apt:_check_timeout() then
                        break
                    end
                end

                fan.sleep(CHECK_TIMEOUT_DURATION)
            end
        end
    )()

    obj.close = function()
        obj.stop = true

        for clientkey, apt in pairs(obj.clientmap) do
            apt:close()
        end

        obj.serv:close()
        obj.serv = nil
    end

    return obj
end

return {
    connect = connect,
    bind = bind
}
