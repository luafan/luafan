mariadb.orm
===========

LuaFan's Object-Relational Mapping (ORM) module provides a high-level, object-oriented interface for database operations. It automatically handles table creation, data mapping, and provides convenient CRUD operations.

## Overview

The ORM module simplifies database operations by:
- **Automatic table management**: Creates and migrates tables based on schema definitions
- **Object mapping**: Converts database rows to Lua objects with methods
- **Type safety**: Enforces data types and constraints
- **Relationship handling**: Supports foreign keys and relationships
- **Query building**: Provides fluent query interface

## Creating an ORM Context

### `orm.new(connection, schema)`

Creates a new ORM context with table definitions.

**Parameters:**
- `connection`: Active MariaDB connection object
- `schema` (table): Table schema definitions

**Returns:** ORM context object

**Example:**
```lua
local mariadb = require("fan.mariadb")
local orm = require("mariadb.orm")

local conn = mariadb.connect("mydb", "user", "password", "localhost", 3306)

local ctx = orm.new(conn, {
    users = {
        id = "int auto_increment primary key",
        name = "varchar(100) not null",
        email = "varchar(255) unique",
        age = "int default 0",
        created_at = "datetime default current_timestamp",
        updated_at = "datetime default current_timestamp on update current_timestamp"
    },
    posts = {
        id = "int auto_increment primary key",
        user_id = "int not null",
        title = "varchar(200) not null",
        content = "text",
        published = "boolean default false",
        created_at = "datetime default current_timestamp",
        -- Foreign key relationship
        ["foreign key (user_id)"] = "references users(id) on delete cascade"
    }
})
```

## Schema Definition

Table schemas use SQL-like syntax for column definitions:

```lua
{
    column_name = "data_type [constraints]",
    -- Examples:
    id = "int auto_increment primary key",
    name = "varchar(100) not null",
    email = "varchar(255) unique not null",
    age = "int default 18",
    balance = "decimal(10,2) default 0.00",
    active = "boolean default true",
    created_at = "datetime default current_timestamp",
    profile = "json",  -- For JSON columns in MariaDB 10.2+

    -- Indexes and constraints
    ["unique key email_idx"] = "(email)",
    ["index name_idx"] = "(name)",
    ["foreign key (user_id)"] = "references users(id)"
}
```

## CRUD Operations

### Inserting Records

#### `context.tablename("insert"|"new", data)`

Creates a new record in the specified table.

**Parameters:**
- `data` (table): Field values for the new record

**Returns:** Row object with methods, or `nil` on failure

**Example:**
```lua
-- Insert a new user
local user = ctx.users("insert", {
    name = "Alice Johnson",
    email = "alice@example.com",
    age = 28
})

if user then
    print("Created user with ID:", user.id)
    print("User created at:", user.created_at.year, user.created_at.month, user.created_at.day)
end

-- Insert with explicit ID (if not auto_increment)
local admin = ctx.users("new", {
    id = 1,
    name = "Admin User",
    email = "admin@example.com",
    age = 35
})
```

### Querying Records

#### `context.tablename("select"|"list"|"one", [where_clause, ...])`

Retrieves records from the table.

**Parameters:**
- `where_clause` (string, optional): SQL WHERE condition with `?` placeholders
- `...` (mixed, optional): Values for placeholders

**Returns:**
- `"select"` or `"list"`: Array of row objects
- `"one"`: Single row object or `nil`

**Examples:**
```lua
-- Get all users
local all_users = ctx.users("list")
for i, user in ipairs(all_users) do
    print(user.name, user.email)
end

-- Get users with conditions
local active_users = ctx.users("select", "age > ? AND email LIKE ?", 21, "%@gmail.com")

-- Get single user
local user = ctx.users("one", "email = ?", "alice@example.com")
if user then
    print("Found user:", user.name)
end

-- Get with ordering and limits
local recent_users = ctx.users("select", "created_at > ? ORDER BY created_at DESC LIMIT ?",
    "2023-01-01", 10)
```

### Updating Records

#### Row-level updates
Row objects have an `update()` method for saving changes:

```lua
local user = ctx.users("one", "email = ?", "alice@example.com")
if user then
    user.age = 29
    user.name = "Alice Smith"  -- Changed surname
    local success = user:update()
    if success then
        print("User updated successfully")
    end
end
```

#### Bulk updates
Use the context's update method for bulk operations:

```lua
-- Update multiple records
local affected = ctx:update("UPDATE users SET age = age + 1 WHERE age < ?", 30)
print("Updated", affected, "users")
```

### Deleting Records

#### `context.tablename("delete"|"remove", where_clause, ...)`

Deletes records matching the condition.

**Example:**
```lua
-- Delete specific users
local deleted = ctx.users("delete", "age < ? AND created_at < ?", 18, "2022-01-01")
print("Deleted", deleted, "underage users")

-- Delete by ID
ctx.users("remove", "id = ?", 42)
```

#### Row-level deletion
Row objects have `delete()` and `remove()` methods:

```lua
local inactive_user = ctx.users("one", "email = ?", "inactive@example.com")
if inactive_user then
    inactive_user:delete()  -- or inactive_user:remove()
    print("User deleted")
end
```

## Advanced Queries

### Direct SQL Execution

The context provides methods for direct SQL execution when you need more control:

#### `context:select(sql, ...)`
Execute SELECT queries and return row objects:

```lua
local results = ctx:select([[
    SELECT u.name, u.email, COUNT(p.id) as post_count
    FROM users u
    LEFT JOIN posts p ON u.id = p.user_id
    WHERE u.age > ?
    GROUP BY u.id, u.name, u.email
    ORDER BY post_count DESC
]], 25)

for _, row in ipairs(results) do
    print(string.format("%s has %d posts", row.name, row.post_count))
end
```

#### `context:update(sql, ...)`, `context:delete(sql, ...)`, `context:insert(sql, ...)`

Execute modification queries:

```lua
-- Complex update
ctx:update([[
    UPDATE users u
    JOIN posts p ON u.id = p.user_id
    SET u.updated_at = NOW()
    WHERE p.created_at > ?
]], "2023-01-01")

-- Insert with subquery
ctx:insert([[
    INSERT INTO user_stats (user_id, post_count)
    SELECT u.id, COUNT(p.id)
    FROM users u
    LEFT JOIN posts p ON u.id = p.user_id
    GROUP BY u.id
]])
```

## Working with Relationships

When you have foreign key relationships, you can work with related data:

```lua
-- Create a user and posts
local user = ctx.users("insert", {
    name = "Bob Wilson",
    email = "bob@example.com"
})

-- Create posts for this user
local post1 = ctx.posts("insert", {
    user_id = user.id,
    title = "My First Post",
    content = "Hello, world!",
    published = true
})

local post2 = ctx.posts("insert", {
    user_id = user.id,
    title = "Draft Post",
    content = "Work in progress...",
    published = false
})

-- Query related data
local user_posts = ctx.posts("select", "user_id = ? ORDER BY created_at DESC", user.id)
print(string.format("User %s has %d posts", user.name, #user_posts))
```

## Data Types and Validation

The ORM automatically handles type conversion and basic validation:

```lua
-- Define a table with various data types
local ctx = orm.new(conn, {
    products = {
        id = "int auto_increment primary key",
        name = "varchar(100) not null",
        price = "decimal(10,2) not null",
        in_stock = "boolean default true",
        metadata = "json",  -- JSON column
        created_at = "datetime default current_timestamp"
    }
})

-- Insert with type checking
local product = ctx.products("insert", {
    name = "Laptop",
    price = 999.99,  -- Will be properly handled as decimal
    in_stock = true,
    metadata = {brand = "TechCorp", model = "X1"}  -- JSON data
})
```

## Transaction Support

Use the underlying connection for transaction control:

```lua
local conn = ctx._connection  -- Access underlying connection

conn:autocommit(false)
local success, err = pcall(function()
    local user = ctx.users("insert", {name = "Test User", email = "test@example.com"})
    ctx.posts("insert", {user_id = user.id, title = "Test Post", content = "Test content"})
    conn:commit()
end)

if not success then
    conn:rollback()
    error("Transaction failed: " .. err)
end
conn:autocommit(true)
```

## Error Handling

Always check return values for error conditions:

```lua
local user = ctx.users("insert", {
    name = "John Doe",
    email = "invalid-email"  -- This might fail validation
})

if not user then
    print("Failed to create user - check constraints and data")
end

-- For bulk operations
local results = ctx.users("select", "invalid_column = ?", "value")
if not results then
    print("Query failed - check SQL syntax and column names")
end
```

## Best Practices

1. **Define proper schemas**: Include constraints, indexes, and foreign keys
2. **Use transactions**: For operations that modify multiple tables
3. **Validate input**: Check data before insertion
4. **Handle errors**: Always check return values
5. **Close connections**: Properly clean up database resources
6. **Use appropriate data types**: Choose correct MySQL types for your data
7. **Index frequently queried columns**: Add indexes for better performance

## Migration and Schema Changes

When you need to modify your schema:

```lua
-- The ORM will automatically create tables that don't exist
-- For existing tables, you may need manual migration:

-- Check if column exists and add if needed
conn:execute("ALTER TABLE users ADD COLUMN phone VARCHAR(20) DEFAULT NULL")

-- Update your schema definition to match
local updated_ctx = orm.new(conn, {
    users = {
        id = "int auto_increment primary key",
        name = "varchar(100) not null",
        email = "varchar(255) unique",
        phone = "varchar(20)",  -- New column
        age = "int default 0",
        created_at = "datetime default current_timestamp"
    }
})
```
