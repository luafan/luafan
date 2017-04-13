mariadb.pool
============

### `ctxpool = pool.new(...)`

create a new connection context pool. all arguments will be passed to `orm.new(conn, ...)`.

all the mariadb parameters are coming from [config](config.md).

### APIs
* `safe(func, ...)` schedule a context from pool, pass to `func(context, ...)`, return context to pool at the end of `func`, the return value is from `func`.

### Samples
```lua
local ctxpool = pool.new({
  ["hi"] = {
    ["aa"] = "varchar(2)",
    ["bb"] = "varchar(3)",
    ["cc"] = "int(4)",
  }
})

local function list(ctx, cc)
  return ctx.hi("one", "where cc=?", cc)
end

local row = ctxpool:safe(list, 1)
```
