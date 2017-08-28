sqlite3.orm
===========

`local orm = require "sqlite3.orm"`

### `orm.new` create a new context with sqlite3.
```lua
local context = orm.new(<db>, {
  ["tablename"] = <table definition map>,
  ...
})
```

```lua
local db = sqlite3.open("xxx.sqlite")
local blob_model = {
    -- ["id"] = "integer primary key", -- id is not required to define.
    ["path"] = "text",
    ["temppath"] = "text",
    ["name"] = "text",
    ["typeid"] = "integer",
    ["contenttype"] = "text",
    ["md5"] = "text",
    ["size"] = "integer",
    ["status"] = "integer default 0"
}
local block_model = {
    ["blobid"] = "integer",
    ["bg"] = "integer default 0",
    ["ed"] = "integer default 0",
    ["hash"] = "text",
    ["status"] = "integer"
}
local context = orm.new(db, {
    ["blob"] = blob_model,
    ["block"] = block_model
})
```

### insert row
```lua
local row = context.<tablename>("new|insert", {modelmap})
```
```lua
local row = context.blob("new", {
    path = "test",
size = 123 })
```

### update row
```lua
row.path = nil
row.typeid = "2"
row:update()
```

### get all or by condition
```lua
local results = context.<tablename>("list|select", format, ...)
```

* `list`,`select` return table array contains all the rows.
* `one` return only the first row.

```lua
local results = context.blob("list", "where id>? and typeid=?", 40, 2)
for k,v in ipairs(results) do
    print(v.id)
end
```

### delete
```lua
context.<tablename>("delete|remove", format, ...)
-- or
row:delete() or row:remove()
```
```lua
context.blob("delete", "id=? and typeid=?", 40, 2)
```

### sql
```lua
context:<select|update|delete|insert>(sql, ...)
```
```lua
local results = context:select("select * from aa,bb where aa.id=bb.id and
aa.cc=?", 22)
context:update("update aa where cc=?", 22)
```
