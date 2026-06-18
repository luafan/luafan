-- Shared ORM base module
-- Provides common table/field metatable logic for both MariaDB and SQLite3 backends.
-- Each backend provides an adapter table with backend-specific operations.

local setmetatable = setmetatable
local getmetatable = getmetatable
local pairs = pairs
local type = type
local table = table
local string = string
local ipairs = ipairs
local error = error

local KEY_CONTEXT = "^context"
local KEY_TABLE = "^table"
local KEY_ATTR = "^attr"
local KEY_MODEL = "^model"
local KEY_NAME = "^name"

local FIELD_ID_DEFAULT = "id"

local function maxn(t)
  local n = 0
  for k, v in pairs(t) do
    if k > n then
      n = k
    end
  end
  return n
end

-- Create an ORM module for a given backend adapter.
--
-- adapter fields:
--   prepare(db, sql)                       -> stmt
--   bind_values(stmt, ...)                 -> void
--   execute_stmt(stmt)                     -> result  (execute stmt, close on error)
--   fetch_rows(t, stmt)                    -> {row, ...}
--   each_rows(t, stmt, func)               -> void
--   close_stmt(stmt)                       -> void
--   delete_row(db, tablename, field_id, id_value) -> result
--   get_last_id(db)                        -> number|nil
--   BUILTIN_VALUE_NOW                      -> string|nil
--   FIELD_ID_KEY                           -> table|nil
--   update_schema(ctx, db, tablename, model) -> void
--   post_table_init(ctx, db, tablename, t)   -> void  (optional, e.g. column ordering)
--   readonly_supported                     -> boolean
--   limit_one                              -> string  (" limit 1" or " limit 0,1")
--   ctx_select_rows(ctx, db, stmt)         -> {row, ...}  (for ctx.select)
--   ctx_exec(ctx, db, stmt)                -> result  (for ctx.update/delete/insert)
--   insert_handle_long_data                -> boolean  (MariaDB LONG_DATA support)
local function create(adapter)

  local FIELD_ID_KEY = adapter.FIELD_ID_KEY
  local BUILTIN_VALUE_NOW = adapter.BUILTIN_VALUE_NOW

  local function delete(ctx, db, tablename, fmt, ...)
    local stmt
    if fmt then
      stmt = adapter.prepare(db, string.format("delete from %s %s", tablename, fmt))
      adapter.bind_values(stmt, ...)
    else
      stmt = adapter.prepare(db, string.format("delete from %s", tablename))
    end
    local st = adapter.execute_stmt(stmt)
    adapter.close_stmt(stmt)
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
              local st = adapter.delete_row(db, t[KEY_NAME], FIELD_ID, attr[FIELD_ID])
              if st then
                setmetatable(r, nil)
                r[KEY_ATTR] = nil
              end
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
              if #list > 0 then
                local db = getmetatable(ctx).db
                local stmt = adapter.prepare(db,
                  "update " .. t[KEY_NAME] .. " set " .. table.concat(list, ",") ..
                  " where " .. FIELD_ID .. "=?")
                table.insert(values, attr[FIELD_ID])
                adapter.bind_values(stmt, table.unpack(values, 1, maxn(values)))
                adapter.execute_stmt(stmt)
                adapter.close_stmt(stmt)
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
    return adapter.fetch_rows(t, stmt, make_row_mt)
  end

  local function each_rows(t, stmt, func)
    return adapter.each_rows(t, stmt, func, make_row_mt)
  end

  local field_mt = {
    __index = function(f, key) end,
    __call = function(f, fmt, ...)
      local t = f[KEY_TABLE]
      local ctx = t[KEY_CONTEXT]
      local db = getmetatable(ctx).db
      local stmt
      if fmt then
        stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME] .. " where " .. f[KEY_NAME] .. fmt)
        adapter.bind_values(stmt, ...)
      else
        stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME])
      end
      local lines = make_rows(t, stmt)
      adapter.close_stmt(stmt)
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
        local suffix = (key == "one") and adapter.limit_one or ""
        if fmt then
          local params = {...}
          stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME] .. " " .. fmt .. suffix)
          if #params > 0 then
            adapter.bind_values(stmt, ...)
          end
        else
          stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME] .. suffix)
        end
        local lines = make_rows(t, stmt)
        adapter.close_stmt(stmt)
        if key == "one" then
          return #lines > 0 and lines[1] or nil
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
          stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME] .. " " .. fmt)
          if #params > 0 then
            adapter.bind_values(stmt, ...)
          end
        else
          stmt = adapter.prepare(db, "select * from " .. t[KEY_NAME])
        end
        each_rows(t, stmt, key)
        adapter.close_stmt(stmt)
      elseif key == "delete" or key == "remove" then
        local fmt = obj
        local db = getmetatable(t[KEY_CONTEXT]).db
        return delete(ctx, db, t[KEY_NAME], fmt, ...)
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
            if BUILTIN_VALUE_NOW and vv == BUILTIN_VALUE_NOW then
              table.insert(places, vv)
            elseif adapter.insert_handle_long_data and type(vv) == "function" then
              table.insert(places, "?")
              table.insert(values, adapter.LONG_DATA_SENTINEL)
              handles[vv] = #places - 1
            else
              table.insert(places, "?")
              table.insert(values, vv)
            end
          end
        end
        if #keys == 0 then
          return nil
        end
        local ctx = t[KEY_CONTEXT]
        local db = getmetatable(ctx).db
        local stmt = adapter.prepare(db,
          "insert into " .. t[KEY_NAME] ..
          " (" .. table.concat(keys, ",") .. ") values(" .. table.concat(places, ",") .. ")")
        adapter.bind_values(stmt, table.unpack(values, 1, maxn(values)))
        for vv, idx in pairs(handles) do
          local st, msg = pcall(vv, stmt, idx)
          if not st then
            print(msg)
          end
        end
        adapter.execute_stmt(stmt)
        adapter.close_stmt(stmt)
        local last_insert_rowid = adapter.get_last_id(db)
        if last_insert_rowid then
          local attr = {}
          local r = {}
          for i, v in ipairs(keys) do
            r[v] = values[i]
            attr[v] = values[i]
          end
          local FIELD_ID = model[FIELD_ID_KEY] or FIELD_ID_DEFAULT
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

  local function new(db, models)
    local ctx = adapter.create_ctx()
    local mt = {
      db = db,
      models = models,
      row_mt_map = {},
      __index = function(ctx, key)
        if key == "select" then
          return function(ctx, fmt, ...)
            local db = getmetatable(ctx).db
            local stmt = adapter.prepare(db, fmt)
            adapter.bind_values(stmt, ...)
            local lines = adapter.ctx_select_rows(ctx, db, stmt)
            adapter.close_stmt(stmt)
            return lines
          end
        elseif key == "update" or key == "delete" or key == "insert" then
          return function(ctx, fmt, ...)
            local db = getmetatable(ctx).db
            local stmt = adapter.prepare(db, fmt)
            adapter.bind_values(stmt, ...)
            return adapter.ctx_exec(ctx, db, stmt)
          end
        end
      end
    }
    setmetatable(ctx, mt)

    for k, v in pairs(models) do
      local t = {
        [KEY_NAME] = k,
        [KEY_MODEL] = v,
        [KEY_CONTEXT] = ctx
      }
      adapter.update_schema(ctx, db, k, v)
      if adapter.post_table_init then
        adapter.post_table_init(ctx, db, k, t)
      end
      setmetatable(t, table_mt)
      ctx[k] = t
    end

    return ctx
  end

  return {
    new = new,
    BUILTIN_VALUE_NOW = BUILTIN_VALUE_NOW,
    FIELD_ID_KEY = FIELD_ID_KEY,
    KEY_CONTEXT = KEY_CONTEXT,
    KEY_TABLE = KEY_TABLE,
    KEY_ATTR = KEY_ATTR,
    KEY_MODEL = KEY_MODEL,
    KEY_NAME = KEY_NAME,
  }
end

return {
  create = create,
  KEY_CONTEXT = KEY_CONTEXT,
  KEY_TABLE = KEY_TABLE,
  KEY_ATTR = KEY_ATTR,
  KEY_MODEL = KEY_MODEL,
  KEY_NAME = KEY_NAME,
  FIELD_ID_DEFAULT = FIELD_ID_DEFAULT,
  maxn = maxn,
}
