local setmetatable = setmetatable
local getmetatable = getmetatable
local pairs = pairs
local string = string
local type = type
local table = table
local print = print
local next = next
local ipairs = ipairs
local error = error
local math = math

local mariadb = require "fan.mariadb"

local config = require "config"

local KEY_CONTEXT = "^context"
local KEY_TABLE = "^table"
local KEY_ATTR = "^attr"
local KEY_MODEL = "^model"
local KEY_NAME = "^name"
local KEY_ORDER = "^order"

local FIELD_ID_DEFAULT = "id"

local BUILTIN_VALUE_NOW = "NOW()"
local FIELD_ID_KEY = {}

local function maxn(t)
  local n = 0
  for k, v in pairs(t) do
    if k > n then
      n = k
    end
  end

  return n
end

local function prepare(ctx, db, sql)
  if config.debug then
    print("prepare", sql)
  end
  return assert(db:prepare(sql))
end

local function stmt_close(stmt)
  stmt:close()
end

local function stmt_execute(stmt)
  local result, msg = stmt:execute()
  if not result then
    stmt_close(stmt)
    error(msg)
  else
    return result
  end
end

local function bind_values(stmt, ...)
  if config.debug then
    print("bind_values", ...)
  end

  if not stmt then
    print(debug.traceback())
  end

  stmt:bind_param(...)
end

local function execute(db, ...)
  if config.debug then
    print("execute", ...)
  end
  assert(db:execute(...))
end

local function delete(ctx, db, tablename, fmt, ...)
  local stmt = nil
  if fmt then
    stmt = prepare(ctx, db, string.format("delete from %s where %s", tablename, fmt))
    bind_values(stmt, ...)
  else
    stmt = prepare(ctx, db, string.format("delete from %s", tablename))
  end
  local st = stmt_execute(stmt)
  stmt_close(stmt)
  return st
end

local function make_row_mt(t)
  local ctx = t[KEY_CONTEXT]
  local FIELD_ID = t[KEY_MODEL][FIELD_ID_KEY] or FIELD_ID_DEFAULT
  local ctx_mt = getmetatable(ctx)

  local row_mt = ctx_mt.row_mt_map[t]
  if not row_mt then
    row_mt = {
      __index = function(r, key)
        if key == "delete" or key == "remove" then
          local func = function(r)
            local attr = r[KEY_ATTR]

            local db = getmetatable(t[KEY_CONTEXT]).db
            local st = delete(ctx, db, t[KEY_NAME], FIELD_ID .. "=?", attr[FIELD_ID])
            setmetatable(r, nil)
            r[KEY_ATTR] = nil
            return st
          end

          r[key] = func
          return func
        elseif key == "update" then
          local func = function(r)
            local attr = r[KEY_ATTR]

            local list = {}
            local keys = {}
            local values = {}
            for k, v in pairs(t[KEY_MODEL]) do
              if type(v) ~= "function" and r[k] ~= attr[k] then
                if not r[k] then
                  table.insert(list, string.format("%s=null", k))
                else
                  table.insert(list, string.format("%s=?", k))
                  table.insert(values, r[k])
                end

                table.insert(keys, k)
              end
            end

            if #(list) > 0 then
              local db = getmetatable(ctx).db
              local stmt =
                prepare(
                ctx,
                db,
                "update " .. t[KEY_NAME] .. " set " .. table.concat(list, ",") .. " where " .. FIELD_ID .. "=?"
              )

              table.insert(values, attr[FIELD_ID])
              bind_values(stmt, table.unpack(values, 1, maxn(values)))

              stmt_execute(stmt)
              stmt_close(stmt)

              for i, k in ipairs(keys) do
                attr[k] = r[k]
              end
            end
          end

          r[key] = func
          return func
        else
          local handler = t[KEY_MODEL][key]
          if type(handler) == "function" then
            return handler(t[KEY_CONTEXT], r, key)
          end
        end
      end
    }

    row_mt[KEY_TABLE] = t

    ctx_mt.row_mt_map[t] = row_mt
  end

  return row_mt
end

local function make_rows(t, stmt)
  local lines = {}
  while true do
    local row = stmt:fetch()

    if not row then
      break
    end

    if not t[KEY_CONTEXT]._readonly then
      local attr = {}
      for k, v in pairs(row) do
        attr[k] = v
      end

      row[KEY_ATTR] = attr
      setmetatable(row, make_row_mt(t))
    end

    table.insert(lines, row)
  end

  return lines
end

local function each_rows(t, stmt, eachfunc)
  local order = t[KEY_ORDER]
  while true do
    local row = stmt:fetch()

    if not row then
      break
    end

    if not t[KEY_CONTEXT]._readonly then
      local attr = {}
      for k, v in pairs(row) do
        attr[k] = v
      end

      row[KEY_ATTR] = attr
      setmetatable(row, make_row_mt(t))
    end

    eachfunc(row)
  end

  return lines
end

local field_mt = {
  __index = function(f, key)
  end,
  __call = function(f, fmt, ...)
    local t = f[KEY_TABLE]
    local ctx = t[KEY_CONTEXT]
    local db = getmetatable(ctx).db
    local stmt
    if fmt then
      stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME] .. " where " .. f[KEY_NAME] .. fmt)
      bind_values(stmt, ...)
    else
      stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME])
    end

    stmt_execute(stmt)
    local lines = make_rows(t, stmt)
    stmt_close(stmt)

    return lines
  end
}

local table_mt = {
  __index = function(t, key)
    if t[KEY_MODEL][key] then
      local f = {
        [KEY_NAME] = key,
        [KEY_TABLE] = t
      }

      setmetatable(f, field_mt)
      t[key] = f
      return f
    end
  end,
  __call = function(t, key, obj, ...)
    if key == "select" or key == "list" or key == "one" then
      local ctx = t[KEY_CONTEXT]
      local db = getmetatable(ctx).db
      local stmt
      local fmt = obj
      local suffix
      if key == "one" then
        suffix = " limit 0,1"
      else
        suffix = ""
      end

      if fmt then
        local params = {...}
        stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME] .. " " .. fmt .. suffix)
        if #(params) > 0 then
          bind_values(stmt, ...)
        end
      else
        stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME] .. suffix)
      end
      stmt_execute(stmt)
      local lines = make_rows(t, stmt)
      stmt_close(stmt)
      if key == "one" then
        return #(lines) > 0 and lines[1] or nil
      else
        return lines
      end
    elseif type(key) == "function" then
      local ctx = t[KEY_CONTEXT]
      local db = getmetatable(ctx).db
      local stmt
      local fmt = obj

      if fmt then
        local params = {...}
        stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME] .. " " .. fmt)
        if #(params) > 0 then
          bind_values(stmt, ...)
        end
      else
        stmt = prepare(ctx, db, "select * from " .. t[KEY_NAME])
      end
      stmt_execute(stmt)
      each_rows(t, stmt, key)
      stmt_close(stmt)
    elseif key == "delete" or key == "remove" then
      local fmt = obj

      local db = getmetatable(t[KEY_CONTEXT]).db
      local st = delete(ctx, db, t[KEY_NAME], fmt, ...)

      return st
    elseif key == "new" or key == "insert" then
      local map = obj
      if type(map) ~= "table" then
        return nil
      end

      local model = t[KEY_MODEL]

      local keys = {}
      local places = {}
      local values = {}

      local handles = {}

      for k, v in pairs(model) do
        local vv = map[k]
        if vv then
          table.insert(keys, k)
          if vv == BUILTIN_VALUE_NOW then
            table.insert(places, vv)
          elseif type(vv) == "function" then
            table.insert(places, "?")
            table.insert(values, mariadb.LONG_DATA)
            handles[vv] = #(places) - 1
          else
            table.insert(places, "?")
            table.insert(values, vv)
          end
        end
      end

      if #(keys) == 0 then
        return nil
      end

      local ctx = t[KEY_CONTEXT]
      local db = getmetatable(ctx).db

      local stmt =
        prepare(
        ctx,
        db,
        "insert into " ..
          t[KEY_NAME] .. " (" .. table.concat(keys, ",") .. ") values(" .. table.concat(places, ",") .. ")"
      )
      bind_values(stmt, table.unpack(values, 1, maxn(values)))
      for vv, idx in pairs(handles) do
        local st, msg = pcall(vv, stmt, idx)
        if not st then
          print(msg)
        end
      end
      stmt_execute(stmt)
      stmt_close(stmt)

      local last_insert_rowid = db:getlastautoid()
      if config.debug then
        print("last_insert_rowid", last_insert_rowid)
      end

      if last_insert_rowid then
        local attr = {}
        local r = {}
        for i, v in ipairs(keys) do
          r[v] = values[i]
          attr[v] = values[i]
        end

        local FIELD_ID = t[KEY_MODEL][FIELD_ID_KEY] or FIELD_ID_DEFAULT

        r[FIELD_ID] = last_insert_rowid
        attr[FIELD_ID] = last_insert_rowid
        r[KEY_ATTR] = attr
        setmetatable(r, make_row_mt(t))

        return r
      else
        return nil
      end
    end
  end
}

local function update_schema(ctx, db, tablename, model)
  local table_exist = false
  local cur = assert(db:execute("show tables"))
  while true do
    local row = cur:fetch()
    if not row or table_exist then
      break
    else
      for k, v in pairs(row) do
        if v == tablename then
          table_exist = true
          break
        end
      end
    end
  end
  cur:close()

  local currColnames = {}

  if table_exist then
    local cur = assert(db:execute(string.format("show columns from %s", tablename)))

    while true do
      local row = cur:fetch()
      if not row then
        break
      else
        table.insert(currColnames, row.Field)
      end
    end

    cur:close()
  end

  if #(currColnames) > 0 then
    for k, v in pairs(model) do
      if type(k) == "string" and type(v) == "string" then
        local found = false
        for i, name in ipairs(currColnames) do
          if name == k then
            found = true
            break
          end
        end

        if not found then
          execute(db, string.format("ALTER TABLE %s ADD `%s` %s", tablename, k, v))
        end
      end
    end
  else
    local items = {}

    local FIELD_ID = model[FIELD_ID_KEY] or FIELD_ID_DEFAULT

    if not model[FIELD_ID] then
      model[FIELD_ID] = "bigint primary key auto_increment not null"
    end

    for k, v in pairs(model) do
      if type(v) == "string" and type(k) == "string" then
        table.insert(items, string.format("`%s` %s", k, v))
      end
    end

    for i, v in ipairs(model) do
      if type(v) == "string" then
        table.insert(items, v)
      end
    end

    execute(
      db,
      string.format(
        "CREATE TABLE IF NOT EXISTS `%s` (%s) ENGINE=MyISAM DEFAULT CHARSET=utf8;",
        tablename,
        table.concat(items, ", ")
      )
    )
  end
end

local function new(db, models)
  local ctx = {
    _readonly = false
  }

  local mt = {
    db = db,
    models = models,
    row_mt_map = {},
    __index = function(ctx, key)
      if key == "select" then
        return function(ctx, fmt, ...)
          local db = getmetatable(ctx).db
          local stmt = prepare(ctx, db, fmt)
          bind_values(stmt, ...)
          stmt_execute(stmt)

          local lines = {}
          while true do
            local row = stmt:fetch()
            if row then
              table.insert(lines, row)
            else
              break
            end
          end

          stmt_close(stmt)
          return lines
        end
      elseif key == "update" or key == "delete" or key == "insert" then
        return function(ctx, fmt, ...)
          local db = getmetatable(ctx).db
          local stmt = prepare(ctx, db, fmt)
          bind_values(stmt, ...)
          stmt_execute(stmt)
          stmt_close(stmt)

          if config.debug and key == "insert" then
            print("last_insert_rowid", db:getlastautoid())
          end

          return db:getlastautoid()
        end
      end
    end
  }
  setmetatable(ctx, mt)

  for k, v in pairs(models) do
    local order = {}
    local t = {
      [KEY_NAME] = k,
      [KEY_MODEL] = v,
      [KEY_CONTEXT] = ctx,
      [KEY_ORDER] = order
    }

    update_schema(ctx, db, k, v)

    local cur = assert(db:execute(string.format("show columns from %s", k)))
    while true do
      local row = cur:fetch()
      if not row then
        break
      else
        table.insert(order, row.Field)
      end
    end
    cur:close()

    setmetatable(t, table_mt)

    ctx[k] = t
  end

  return ctx
end

return {
  new = new,
  BUILTIN_VALUE_NOW = BUILTIN_VALUE_NOW,
  FIELD_ID_KEY = FIELD_ID_KEY
}
