local ok_mariadb, mariadb = pcall(require, "fan.mariadb")
if not ok_mariadb then mariadb = nil end

local config = require "config"
local orm_base = require "fan.orm_base"

local KEY_ORDER = "^order"
local BUILTIN_VALUE_NOW = "NOW()"
local FIELD_ID_KEY = {}

local function make_adapter()
  local adapter = {}

  adapter.BUILTIN_VALUE_NOW = BUILTIN_VALUE_NOW
  adapter.FIELD_ID_KEY = FIELD_ID_KEY
  adapter.limit_one = " limit 0,1"
  adapter.readonly_supported = true
  adapter.insert_handle_long_data = true
  adapter.LONG_DATA_SENTINEL = mariadb and mariadb.LONG_DATA

  function adapter.create_ctx()
    return { _readonly = false }
  end

  function adapter.prepare(db, sql)
    if config.debug then
      print("prepare", sql)
    end
    return assert(db:prepare(sql))
  end

  function adapter.bind_values(stmt, ...)
    if config.debug then
      print("bind_values", ...)
    end
    if not stmt then
      print(debug.traceback())
    end
    assert(stmt:bind_param(...))
  end

  function adapter.execute_stmt(stmt)
    local result, msg = stmt:execute()
    if not result then
      stmt:close()
      error(msg)
    end
    return result
  end

  function adapter.fetch_rows(t, stmt, make_row_mt)
    local lines = {}
    while true do
      local row = stmt:fetch()
      if not row then
        break
      end
      if not t[orm_base.KEY_CONTEXT]._readonly then
        local attr = {}
        for k, v in pairs(row) do
          attr[k] = v
        end
        row[orm_base.KEY_ATTR] = attr
        setmetatable(row, make_row_mt(t))
      end
      table.insert(lines, row)
    end
    return lines
  end

  function adapter.each_rows(t, stmt, func, make_row_mt)
    while true do
      local row = stmt:fetch()
      if not row then
        break
      end
      if not t[orm_base.KEY_CONTEXT]._readonly then
        local attr = {}
        for k, v in pairs(row) do
          attr[k] = v
        end
        row[orm_base.KEY_ATTR] = attr
        setmetatable(row, make_row_mt(t))
      end
      func(row)
    end
  end

  function adapter.close_stmt(stmt)
    stmt:close()
  end

  function adapter.delete_row(db, tablename, field_id, id_value)
    local stmt = assert(db:prepare(
      string.format("delete from %s where %s=?", tablename, field_id)))
    assert(stmt:bind_param(id_value))
    local result, msg = stmt:execute()
    stmt:close()
    if not result then
      error(msg)
    end
    return result
  end

  function adapter.get_last_id(db)
    return db:getlastautoid()
  end

  function adapter.update_schema(ctx, db, tablename, model)
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

    if #currColnames > 0 then
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
            assert(db:execute(string.format("ALTER TABLE %s ADD `%s` %s", tablename, k, v)))
          end
        end
      end
    else
      local items = {}
      local FIELD_ID = model[FIELD_ID_KEY] or orm_base.FIELD_ID_DEFAULT
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
      assert(db:execute(string.format(
        "CREATE TABLE IF NOT EXISTS `%s` (%s) ENGINE=MyISAM DEFAULT CHARSET=utf8;",
        tablename, table.concat(items, ", "))))
    end
  end

  function adapter.post_table_init(ctx, db, tablename, t)
    local order = {}
    local cur = assert(db:execute(string.format("show columns from %s", tablename)))
    while true do
      local row = cur:fetch()
      if not row then
        break
      else
        table.insert(order, row.Field)
      end
    end
    cur:close()
    t[KEY_ORDER] = order
  end

  function adapter.ctx_select_rows(ctx, db, stmt)
    local lines = {}
    while true do
      local row = stmt:fetch()
      if row then
        table.insert(lines, row)
      else
        break
      end
    end
    return lines
  end

  function adapter.ctx_exec(ctx, db, stmt)
    local result, msg = stmt:execute()
    stmt:close()
    if not result then
      error(msg)
    end
    if config.debug then
      print("last_insert_rowid", db:getlastautoid())
    end
    return db:getlastautoid()
  end

  return adapter
end

local mod = orm_base.create(make_adapter())
mod.BUILTIN_VALUE_NOW = BUILTIN_VALUE_NOW
mod.FIELD_ID_KEY = FIELD_ID_KEY

return mod
