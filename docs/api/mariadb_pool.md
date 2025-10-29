mariadb.pool
============

LuaFan's connection pool module provides efficient database connection management for high-concurrency applications. It automatically handles connection allocation, reuse, and cleanup while integrating seamlessly with the ORM layer.

## Overview

The connection pool provides:
- **Connection reuse**: Maintains a pool of active connections to reduce setup overhead
- **Concurrency safety**: Thread-safe connection allocation in coroutine environments
- **Automatic cleanup**: Handles connection lifecycle and resource management
- **Load balancing**: Distributes database load across multiple connections
- **Configuration management**: Uses centralized configuration for connection parameters

## Creating a Connection Pool

### `pool.new(schema, [config])`

Creates a new connection pool with ORM context.

**Parameters:**
- `schema` (table): Table schema definitions (same as `orm.new`)
- `config` (table, optional): Connection pool configuration

**Returns:** Connection pool object

**Configuration:**
Connection parameters are typically loaded from [config](config.md). Default configuration includes:

```lua
{
    mariadb = {
        host = "localhost",
        port = 3306,
        database = "myapp",
        user = "dbuser",
        password = "dbpass",
        charset = "utf8mb4",
        pool_size = 10,        -- Maximum connections in pool
        timeout = 30,          -- Connection timeout in seconds
        retry_count = 3        -- Connection retry attempts
    }
}
```

**Example:**
```lua
local pool = require("mariadb.pool")

-- Create pool with schema definition
local db_pool = pool.new({
    users = {
        id = "int auto_increment primary key",
        name = "varchar(100) not null",
        email = "varchar(255) unique not null",
        created_at = "datetime default current_timestamp"
    },
    posts = {
        id = "int auto_increment primary key",
        user_id = "int not null",
        title = "varchar(200) not null",
        content = "text",
        published = "boolean default false",
        ["foreign key (user_id)"] = "references users(id)"
    }
})
```

## Pool Operations

### `pool:safe(func, ...)`

Executes a function with a database context from the pool. The connection is automatically returned to the pool when the function completes.

**Parameters:**
- `func` (function): Function to execute with database context
- `...` (mixed): Additional arguments passed to the function

**Returns:** Return value from the executed function

**Function Signature:** `func(context, ...)`
- `context`: ORM context with database connection
- `...`: Additional arguments passed through from `pool:safe()`

**Example:**
```lua
-- Define database operations as functions
local function get_user_by_email(ctx, email)
    return ctx.users("one", "email = ?", email)
end

local function create_user_with_post(ctx, user_data, post_data)
    -- This operation uses a single connection for both operations
    local user = ctx.users("insert", user_data)
    if user then
        post_data.user_id = user.id
        local post = ctx.posts("insert", post_data)
        return user, post
    end
    return nil
end

-- Use the pool
local user = db_pool:safe(get_user_by_email, "alice@example.com")
if user then
    print("Found user:", user.name)
end

-- Complex operation with multiple tables
local new_user, new_post = db_pool:safe(create_user_with_post,
    {name = "Bob", email = "bob@example.com"},
    {title = "My First Post", content = "Hello world!", published = true}
)
```

## Advanced Usage Patterns

### Batch Operations

Use the pool for efficient batch processing:

```lua
local function process_user_batch(ctx, user_emails)
    local results = {}

    for i, email in ipairs(user_emails) do
        local user = ctx.users("one", "email = ?", email)
        if user then
            -- Update user activity
            user.last_login = os.date("%Y-%m-%d %H:%M:%S")
            user:update()

            -- Get user's post count
            local posts = ctx.posts("select", "user_id = ?", user.id)
            results[i] = {
                user = user,
                post_count = #posts
            }
        end
    end

    return results
end

-- Process multiple users in a single connection
local user_stats = db_pool:safe(process_user_batch, {
    "alice@example.com",
    "bob@example.com",
    "charlie@example.com"
})

for _, stat in ipairs(user_stats) do
    print(string.format("%s has %d posts", stat.user.name, stat.post_count))
end
```

### Transaction Management

Handle transactions within pool operations:

```lua
local function transfer_posts(ctx, from_user_id, to_user_id, post_ids)
    local conn = ctx._connection

    conn:autocommit(false)
    local success, err = pcall(function()
        -- Verify both users exist
        local from_user = ctx.users("one", "id = ?", from_user_id)
        local to_user = ctx.users("one", "id = ?", to_user_id)

        if not from_user or not to_user then
            error("One or both users not found")
        end

        -- Transfer posts
        for _, post_id in ipairs(post_ids) do
            local post = ctx.posts("one", "id = ? AND user_id = ?", post_id, from_user_id)
            if post then
                post.user_id = to_user_id
                post:update()
            end
        end

        conn:commit()
        return true
    end)

    if not success then
        conn:rollback()
        error("Transfer failed: " .. tostring(err))
    end

    conn:autocommit(true)
    return success
end

-- Execute transaction through pool
local success = db_pool:safe(transfer_posts, 1, 2, {101, 102, 103})
print("Transfer", success and "successful" or "failed")
```

### Error Handling and Retries

The pool automatically handles connection failures and retries:

```lua
local function robust_operation(ctx, data)
    -- The pool handles connection failures automatically
    -- Focus on business logic errors

    local user = ctx.users("insert", data)
    if not user then
        error("Failed to create user - data validation error")
    end

    return user
end

-- Pool will retry connection failures automatically
local success, result = pcall(db_pool.safe, db_pool, robust_operation, {
    name = "Test User",
    email = "test@example.com"
})

if not success then
    print("Operation failed after retries:", result)
else
    print("User created:", result.name)
end
```

## Configuration Management

### Using External Configuration

Load connection settings from configuration files:

```lua
-- config.lua
return {
    mariadb = {
        host = os.getenv("DB_HOST") or "localhost",
        port = tonumber(os.getenv("DB_PORT")) or 3306,
        database = os.getenv("DB_NAME") or "myapp",
        user = os.getenv("DB_USER") or "dbuser",
        password = os.getenv("DB_PASS") or "dbpass",
        charset = "utf8mb4",
        pool_size = 20,
        timeout = 30
    }
}

-- main.lua
local config = require("config")
local pool = require("mariadb.pool")

-- Pool automatically uses config.mariadb settings
local db_pool = pool.new(schema_definition)
```

### Environment-Specific Configuration

```lua
local env = os.getenv("ENVIRONMENT") or "development"

local config = {
    development = {
        mariadb = {
            host = "localhost",
            database = "myapp_dev",
            pool_size = 5
        }
    },
    production = {
        mariadb = {
            host = "prod-db.example.com",
            database = "myapp_prod",
            pool_size = 50,
            timeout = 60
        }
    }
}

-- Set configuration before creating pool
local fan_config = require("fan.config")
fan_config.load(config[env])

local db_pool = pool.new(schema_definition)
```

## Performance Optimization

### Pool Sizing

Choose appropriate pool size based on your application:

```lua
-- For web applications
local config = {
    mariadb = {
        pool_size = math.max(10, concurrent_users / 5),  -- Rule of thumb
        timeout = 30
    }
}

-- For background processing
local config = {
    mariadb = {
        pool_size = worker_count * 2,  -- More connections for workers
        timeout = 300  -- Longer timeout for batch operations
    }
}
```

### Connection Monitoring

Monitor pool usage for optimization:

```lua
local function get_user_stats(ctx)
    local start_time = os.clock()

    local users = ctx.users("select", "created_at > ?", "2023-01-01")
    local query_time = os.clock() - start_time

    print(string.format("Query took %0.3f seconds, returned %d users",
        query_time, #users))

    return users
end

-- Monitor performance
local users = db_pool:safe(get_user_stats)
```

## Best Practices

1. **Use pools for all database access**: Don't create direct connections in production
2. **Keep operations atomic**: Group related operations in single `safe()` calls
3. **Handle errors gracefully**: Pool operations can fail, always check results
4. **Configure appropriate pool size**: Balance memory usage with concurrency needs
5. **Use transactions wisely**: Group multiple operations that must succeed together
6. **Monitor performance**: Track query times and pool utilization
7. **Clean shutdown**: Allow pools to close gracefully on application exit

## Error Scenarios

### Connection Pool Exhaustion

```lua
-- If all connections are in use, safe() will wait or timeout
local function long_running_operation(ctx)
    -- Avoid long-running operations that tie up connections
    ctx.users("select", "age > ?", 18)

    -- Instead of sleeping in the database function:
    -- os.execute("sleep 10")  -- DON'T DO THIS

    return "completed"
end

-- Better: split long operations
local function quick_db_operation(ctx)
    return ctx.users("select", "age > ?", 18)
end

local users = db_pool:safe(quick_db_operation)
-- Do processing outside of pool context
process_users(users)
```

### Connection Failures

The pool handles most connection failures automatically:

```lua
-- Pool will automatically:
-- 1. Detect failed connections
-- 2. Remove them from the pool
-- 3. Create new connections as needed
-- 4. Retry operations on connection failure

local function resilient_query(ctx)
    -- Your code doesn't need to handle connection failures
    return ctx.users("list")
end

-- This will work even if some connections fail
local users = db_pool:safe(resilient_query)
```

## Integration with Web Frameworks

### HTTP Request Handling

```lua
local function handle_user_request(ctx, user_id)
    local user = ctx.users("one", "id = ?", user_id)
    if not user then
        return nil, "User not found"
    end

    local posts = ctx.posts("select", "user_id = ? ORDER BY created_at DESC LIMIT 10", user_id)

    return {
        user = user,
        recent_posts = posts
    }
end

-- In your HTTP handler
local function get_user_profile(request)
    local user_id = tonumber(request.params.id)

    local profile, err = db_pool:safe(handle_user_request, user_id)
    if not profile then
        return {status = 404, body = {error = err}}
    end

    return {status = 200, body = profile}
end
```

This connection pool implementation provides a robust, scalable foundation for database access in LuaFan applications, handling the complexities of connection management while providing a clean, simple API for application developers.
