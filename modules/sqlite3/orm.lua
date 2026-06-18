local sqlite3 = require "lsqlite3"
local orm_base = require "fan.orm_base"

local function make_adapter()
  local adapter = {}

  adapter.BUILTIN_VALUE_NOW = nil
  adapter.FIELD_ID_KEY = nil
  adapter.limit_one = " limit 1"
  adapter.readonly_supported = false
  adapter.insert_handle_long_data = false

  function adapter.create_ctx()
    return {}
  end

  function adapter.prepare(db, sql)
    local stmt = db:prepare(sql)
    if not stmt then
      error(string.format("%s => %s", sql, db:errmsg()))
    end
    return stmt
  end

  function adapter.bind_values(stmt, ...)
    stmt:bind_values(...)
  end

  function adapter.execute_stmt(stmt)
    local st = stmt:step()
    if st ~= sqlite3.DONE then
      local msg = string.format("step failed: %d", st)
      stmt:finalize()
      error(msg)
    end
    return st
  end

  function adapter.fetch_rows(t, stmt, make_row_mt)
    local lines = {}
    for row in stmt:nrows() do
      local r = {}
      local attr = {}
      for k, v in pairs(row) do
        r[k] = v
        attr[k] = v
      end
      r[orm_base.KEY_ATTR] = attr
      setmetatable(r, make_row_mt(t))
      table.insert(lines, r)
    end
    return lines
  end

  function adapter.each_rows(t, stmt, func, make_row_mt)
    for row in stmt:nrows() do
      local r = {}
      local attr = {}
      for k, v in pairs(row) do
        r[k] = v
        attr[k] = v
      end
      r[orm_base.KEY_ATTR] = attr
      setmetatable(r, make_row_mt(t))
      if func(r) then
        break
      end
    end
  end

  function adapter.close_stmt(stmt)
    stmt:finalize()
  end

  function adapter.delete_row(db, tablename, field_id, id_value)
    local stmt = db:prepare(
      string.format("delete from %s where %s=?", tablename, field_id))
    if not stmt then
      return nil
    end
    stmt:bind_values(id_value)
    local st = stmt:step()
    stmt:finalize()
    if st == sqlite3.DONE then
      return st
    end
    return nil
  end

  function adapter.get_last_id(db)
    return db:last_insert_rowid()
  end

  function adapter.update_schema(ctx, db, tablename, model)
    local currColnames = {}
    for row in db:nrows("PRAGMA table_info(" .. tablename .. ")") do
      table.insert(currColnames, row.name)
    end

    if next(currColnames) ~= nil then
      for k, v in pairs(model) do
        if type(v) == "string" then
          local found = false
          for i, name in ipairs(currColnames) do
            if name == k then
              found = true
              break
            end
          end
          if not found then
            db:execute(string.format("ALTER TABLE %s ADD `%s` %s", tablename, k, v))
          end
        end
      end
    else
      local items = {}
      if not model["id"] then
        model["id"] = "INTEGER PRIMARY KEY AUTOINCREMENT"
      end
      for k, v in pairs(model) do
        if type(v) == "string" then
          table.insert(items, string.format("`%s` %s", k, v))
        end
      end
      db:execute(string.format("CREATE TABLE IF NOT EXISTS `%s` (%s);", tablename, table.concat(items, ", ")))
    end
  end

  function adapter.ctx_select_rows(ctx, db, stmt)
    local lines = {}
    for row in stmt:nrows() do
      table.insert(lines, row)
    end
    return lines
  end

  function adapter.ctx_exec(ctx, db, stmt)
    local st = stmt:step()
    stmt:finalize()
    return st
  end

  return adapter
end

return orm_base.create(make_adapter())
