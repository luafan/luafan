mariadb.orm
===========

### `orm.new` create a new context
```lua
local context = orm.new(<db>, {
  ["tablename"] = <table definition map>,
  ...
})
```
```lua
local mariadb = require "fan.mariadb"
local conn = assert(mariadb.connect("db", "user", "password", "192.168.99.100", 3306))

local ctx = orm.new(conn, {
  ["hi"] = {
    ["aa"] = "varchar(2)",
    ["bb"] = "varchar(3)",
    ["cc"] = "int(4)",
  }
})
```

### insert row
```lua
local row = context.<tablename>("new|insert", {modelmap})
```
```lua
ctx.hi("new", {
  ["aa"] = "tt",
  ["bb"] = "eee",
  ["cc"] = 66
})
```

### get all or by condition & update row
```lua
local results = context.<tablename>("list|select|one", format, ...)
```
* `list`,`select` return table array contains all the rows.
* `one` return only the first row.

```lua
local list = ctx.hi("list")
for i,v in ipairs(list) do
  print(v.aa, v.bb, v.cc)
  v.cc = math.random(100)
  v:update()
end
```

### delete
```lua
context.<tablename>("delete|remove", format, ...)
-- or
row:delete() or row:remove()
```
```lua
context.hi("delete", "id=? and cc=?", 40, 2)
```

### sql
```lua
context:<select|update|delete|insert>(sql, ...)
```
```lua
local list = ctx:select("select * from hi")
for i,v in ipairs(list) do
  print(json.encode(v))
end
```
