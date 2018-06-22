--[[
luarocks install lsqlite3
]]
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
local sqlite3 = require "lsqlite3"

local KEY_CONTEXT = "^context"
local KEY_TABLE = "^table"
local KEY_ATTR = "^attr"
local KEY_MODEL = "^model"
local KEY_NAME = "^name"

local FIELD_ID = "id"

local function maxn(t)
	local n = 0
	for k, v in pairs(t) do
		if k > n then
			n = k
		end
	end

	return n
end

local function prepare(db, sql)
	-- print("prepare", sql)
	local stmt = db:prepare(sql)
	if not stmt then
		error(string.format("%s => %s", sql, db:errmsg()))
	else
		return stmt
	end
end

local function bind_values(stmt, ...)
	-- print("bind_values", ...)
	-- local tb = {...}
	-- for i,v in ipairs(tb) do
	-- 	if v == json.null then
	-- 		tb[i] = nil
	-- 	end
	-- end

	stmt:bind_values(...) -- table.unpack(tb, 1, maxn(tb))
end

local function execute(db, ...)
	-- print("execute", ...)
	db:execute(...)
end

local function delete(db, tablename, fmt, ...)
	local stmt = nil
	if fmt then
		stmt = prepare(db, string.format("delete from %s where %s", tablename, fmt))
		bind_values(stmt, ...)
	else
		stmt = prepare(db, string.format("delete from %s", tablename))
	end
	local st = stmt:step()
	stmt:finalize()
	return st
end

local function make_row_mt(t)
	local ctx = t[KEY_CONTEXT]
	local ctx_mt = getmetatable(ctx)

	local row_mt = ctx_mt.row_mt_map[t]
	if not row_mt then
		row_mt = {
			__index = function(r, key)
				if key == "delete" or key == "remove" then
					local func = function(r)
						local attr = r[KEY_ATTR]

						local db = getmetatable(t[KEY_CONTEXT]).db
						local st = delete(db, t[KEY_NAME], "id=?", attr[FIELD_ID])

						if st == sqlite3.DONE then
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

						if #(list) > 0 then
							local db = getmetatable(ctx).db
							local stmt =
								prepare(db, "update " .. t[KEY_NAME] .. " set " .. table.concat(list, ",") .. " where " .. FIELD_ID .. "=?")

							table.insert(values, attr[FIELD_ID])
							bind_values(stmt, table.unpack(values, 1, maxn(values)))

							if stmt:step() == sqlite3.DONE then
								for i, k in ipairs(keys) do
									attr[k] = r[k]
								end
							end

							stmt:finalize()
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
	for row in stmt:nrows() do
		local r = {
			[KEY_ATTR] = row
		}

		setmetatable(r, make_row_mt(t))

		for k, v in pairs(row) do
			r[k] = v
		end

		table.insert(lines, r)
	end

	return lines
end

local function each_rows(t, stmt, eachfunc)
	local lines = {}
	for row in stmt:nrows() do
		local r = {
			[KEY_ATTR] = row
		}

		setmetatable(r, make_row_mt(t))

		for k, v in pairs(row) do
			r[k] = v
		end

		if eachfunc(r) then
			break
		end
	end
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
			stmt = prepare(db, "select * from " .. t[KEY_NAME] .. " where " .. f[KEY_NAME] .. fmt)
			bind_values(stmt, ...)
		else
			stmt = prepare(db, "select * from " .. t[KEY_NAME])
		end

		local lines = make_rows(t, stmt)

		stmt:finalize()

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
				suffix = " limit 1"
			else
				suffix = ""
			end

			if fmt then
				stmt = prepare(db, "select * from " .. t[KEY_NAME] .. " " .. fmt .. suffix)
				bind_values(stmt, ...)
			else
				stmt = prepare(db, "select * from " .. t[KEY_NAME] .. suffix)
			end
			local lines = make_rows(t, stmt)
			stmt:finalize()

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
				stmt = prepare(db, "select * from " .. t[KEY_NAME] .. " " .. fmt)
				if #(params) > 0 then
					bind_values(stmt, ...)
				end
			else
				stmt = prepare(db, "select * from " .. t[KEY_NAME])
			end
			each_rows(t, stmt, key)
			stmt:finalize()
		elseif key == "delete" or key == "remove" then
			local fmt = obj

			local db = getmetatable(t[KEY_CONTEXT]).db
			local st = delete(db, t[KEY_NAME], fmt, ...)

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

			for k, v in pairs(model) do
				if map[k] then
					table.insert(keys, k)
					table.insert(places, "?")
					table.insert(values, map[k])
				end
			end

			if #(keys) == 0 then
				return nil
			end

			local db = getmetatable(t[KEY_CONTEXT]).db

			local stmt =
				prepare(
				db,
				"insert into " .. t[KEY_NAME] .. " (" .. table.concat(keys, ",") .. ") values(" .. table.concat(places, ",") .. ")"
			)
			bind_values(stmt, table.unpack(values, 1, maxn(values)))

			local last_insert_rowid
			if stmt:step() == sqlite3.DONE then
				last_insert_rowid = db:last_insert_rowid()
			end

			stmt:finalize()

			if last_insert_rowid then
				local attr = {}
				local r = {}
				for i, v in ipairs(keys) do
					r[v] = values[i]
					attr[v] = values[i]
				end

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

local function update_schema(db, tablename, model)
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
					execute(db, string.format("ALTER TABLE %s ADD `%s` %s", tablename, k, v))
				end
			end
		end
	else
		local items = {}

		model[FIELD_ID] = "integer primary key"

		for k, v in pairs(model) do
			if type(v) == "string" then
				table.insert(items, string.format("`%s` %s", k, v))
			end
		end

		execute(db, string.format("CREATE TABLE IF NOT EXISTS `%s` (%s);", tablename, table.concat(items, ", ")))
	end
end

local function new(db, models)
	local ctx = {}
	local mt = {
		db = db,
		models = models,
		row_mt_map = {},
		__index = function(ctx, key)
			if key == "select" then
				return function(ctx, fmt, ...)
					local db = getmetatable(ctx).db
					local stmt = prepare(db, fmt)
					bind_values(stmt, ...)

					local lines = {}
					for row in stmt:nrows() do
						table.insert(lines, row)
					end

					stmt:finalize()
					return lines
				end
			elseif key == "update" or key == "delete" or key == "insert" then
				return function(ctx, fmt, ...)
					local db = getmetatable(ctx).db
					local stmt = prepare(db, fmt)
					bind_values(stmt, ...)
					local st = stmt:step()
					stmt:finalize()

					return st
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

		update_schema(db, k, v)

		setmetatable(t, table_mt)

		ctx[k] = t
	end

	return ctx
end

return {
	new = new
}
