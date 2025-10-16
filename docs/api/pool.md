# fan.pool

Generic resource pooling system for managing limited resources with automatic queueing and lifecycle management. Perfect for connection pools, object pools, or any scenario where you need to limit concurrent resource usage.

## Overview

The pool module provides a coroutine-safe way to manage a limited number of resources. When all resources are in use, requests are automatically queued and resumed when a resource becomes available.

## Functions

### new(arg_map, ...)

Create a new resource pool.

**Parameters:**
- `arg_map` (table, optional): Configuration table with callbacks
  - `onnew` (function): Factory function to create new resources when pool is not at capacity
  - `onbind` (function): Called when a resource is taken from the pool
  - `onunbind` (function): Called when a resource is returned to the pool
- `...` (any): Additional arguments passed to `onbind` callback

**Returns:**
- `pool`: Pool instance

**Example:**
```lua
local pool = require "fan.pool"

-- Simple pool with factory function
local db_pool = pool.new({
    onnew = function(pool_instance)
        return connect_to_database()
    end
})
```

## Pool Methods

### pool:pop()

Get a resource from the pool. If no resources are available and the pool is at capacity, this will yield until a resource becomes available.

**Returns:**
- Resource from the pool (processed through `onbind` if provided)

**Example:**
```lua
local connection = db_pool:pop()
-- Use connection...
db_pool:push(connection)
```

### pool:push(resource)

Return a resource to the pool. This will automatically resume any waiting coroutines if there are queued requests.

**Parameters:**
- `resource` (any): The resource to return to the pool

**Example:**
```lua
local connection = db_pool:pop()
-- ... use connection ...
db_pool:push(connection)  -- Return to pool
```

### pool:safe(func, ...)

Execute a function with a resource from the pool, automatically handling resource cleanup even if the function throws an error.

**Parameters:**
- `func` (function): Function to execute, receives the resource as first parameter
- `...` (any): Additional arguments passed to the function

**Returns:**
- Return value of the function (if successful)

**Example:**
```lua
local result = db_pool:safe(function(connection, query)
    return connection:execute(query)
end, "SELECT * FROM users")
```

## Configuration Options

The pool size is controlled by the `config.pool_size` setting, which defaults to 10 if not specified.

## Usage Examples

### Database Connection Pool

```lua
local pool = require "fan.pool"
local mariadb = require "fan.mariadb"

local db_pool = pool.new({
    onnew = function(pool_instance)
        return mariadb.connect({
            host = "localhost",
            user = "app",
            password = "secret",
            database = "myapp"
        })
    end,
    onbind = function(connection, bind_args)
        -- Reset connection state if needed
        connection:execute("SET autocommit=1")
        return connection
    end,
    onunbind = function(connection)
        -- Clean up connection state
        connection:execute("ROLLBACK")  -- Ensure no pending transactions
        return connection
    end
})

-- Usage with automatic cleanup
db_pool:safe(function(conn)
    local result = conn:execute("SELECT COUNT(*) FROM users")
    print("User count:", result[1][1])
end)
```

### HTTP Client Pool

```lua
local pool = require "fan.pool"
local http = require "fan.http"

local http_pool = pool.new({
    onnew = function(pool_instance)
        return {
            session = http.session(),
            requests_made = 0
        }
    end,
    onbind = function(client_info)
        client_info.requests_made = client_info.requests_made + 1
        return client_info.session
    end,
    onunbind = function(session, client_info)
        -- Recycle connection if too many requests made
        if client_info.requests_made > 100 then
            client_info.session:close()
            return nil  -- Remove from pool
        end
        return client_info
    end
})

-- Make HTTP requests through pool
http_pool:safe(function(session)
    local response = session:get("https://api.example.com/data")
    return response.body
end)
```

### Worker Thread Pool

```lua
local pool = require "fan.pool"
local worker = require "fan.worker"

local worker_pool = pool.new({
    onnew = function(pool_instance)
        return worker.new({
            script = "worker_script.lua"
        })
    end,
    onunbind = function(worker_instance)
        -- Check if worker is still healthy
        if not worker_instance:is_alive() then
            return nil  -- Remove dead worker from pool
        end
        return worker_instance
    end
})

-- Distribute work across worker pool
for i = 1, 100 do
    worker_pool:safe(function(w)
        return w:call("process_data", {id = i})
    end)
end
```

## Key Features

- **Coroutine-safe**: Automatically queues requests when pool is exhausted
- **Automatic lifecycle management**: Resources are created on-demand up to pool limit
- **Error safety**: `safe()` method ensures resources are always returned to pool
- **Flexible callbacks**: Customize resource creation, binding, and cleanup
- **FIFO queuing**: Waiting requests are served in first-in-first-out order

## Implementation Notes

- Pool uses a simple linked list for the waiting queue to ensure O(1) enqueue/dequeue
- Resources are stored in an array for efficient access
- The pool automatically manages coroutine yielding and resuming
- Error handling in callbacks is the responsibility of the caller
- Pool size is global via `config.pool_size` but each pool instance maintains its own resources

## Best Practices

1. **Always use `safe()`** for exception safety when possible
2. **Keep `onbind`/`onunbind` lightweight** as they're called frequently
3. **Handle resource failures** in `onunbind` by returning `nil` to remove bad resources
4. **Set appropriate pool sizes** based on your resource constraints
5. **Monitor pool exhaustion** for performance tuning