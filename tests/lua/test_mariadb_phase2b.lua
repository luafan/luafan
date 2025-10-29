#!/usr/bin/env lua

-- MariaDB Phase 2B Tests - Basic CRUD Operations
-- Tests comprehensive Create, Read, Update, Delete operations
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Phase 2B tests - Basic CRUD Operations...")

-- MariaDB connection configuration
local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

-- Global connection for cleanup
local test_conn = nil

-- Test data
local TEST_USERS = {
    {name = "Alice Johnson", email = "alice@example.com", age = 28, department = "Engineering", salary = 75000.00, active = true},
    {name = "Bob Smith", email = "bob@example.com", age = 35, department = "Marketing", salary = 65000.00, active = true},
    {name = "Charlie Brown", email = "charlie@example.com", age = 42, department = "Sales", salary = 55000.00, active = false},
    {name = "Diana Prince", email = "diana@example.com", age = 31, department = "Engineering", salary = 85000.00, active = true},
    {name = "Eve Wilson", email = "eve@example.com", age = 29, department = "HR", salary = 60000.00, active = true}
}

-- Test suite setup/teardown
local function setup_database()
    print("Connecting to MariaDB...")

    local mariadb = require('fan.mariadb')
    test_conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user, DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)

    if not test_conn then
        error("Failed to connect to MariaDB. Make sure Docker container is running:\ncd tests && ./docker-setup.sh start")
    end

    -- Clean up any existing test tables
    test_conn:execute("DROP TABLE IF EXISTS users")
    test_conn:execute("DROP TABLE IF EXISTS orders")
    test_conn:execute("DROP TABLE IF EXISTS products")

    -- Create test table
    local create_sql = [[
        CREATE TABLE users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255) UNIQUE NOT NULL,
            age INT,
            department VARCHAR(50),
            salary DECIMAL(10,2),
            active BOOLEAN DEFAULT TRUE,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        )
    ]]

    local result = test_conn:execute(create_sql)
    if not result then
        error("Failed to create users table")
    end

    print("✓ MariaDB connection established and tables created")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up database...")
        test_conn:execute("DROP TABLE IF EXISTS users")
        test_conn:execute("DROP TABLE IF EXISTS orders")
        test_conn:execute("DROP TABLE IF EXISTS products")
        test_conn:close()
        test_conn = nil
        print("✓ Database cleanup completed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Phase 2B - Basic CRUD Operations")

-- Test 1: Single INSERT operations
suite:test("single_insert_operations", function()
    local conn = test_conn or setup_database()

    -- Test single INSERT
    local insert_sql = [[
        INSERT INTO users (name, email, age, department, salary, active)
        VALUES ('John Doe', 'john@example.com', 30, 'IT', 70000.00, TRUE)
    ]]

    local result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Verify the insert worked
    local select_result = conn:execute("SELECT * FROM users WHERE email = 'john@example.com'")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.name, "John Doe")
        TestFramework.assert_equal(row.email, "john@example.com")
        TestFramework.assert_equal(row.age, 30)
        TestFramework.assert_equal(row.department, "IT")
        TestFramework.assert_equal(row.salary, 70000.00)
        TestFramework.assert_equal(row.active, 1)
        TestFramework.assert_not_nil(row.created_at)
        select_result:close()
    end

    print("✓ Single INSERT operations test passed")
end)

-- Test 2: Batch INSERT operations
suite:test("batch_insert_operations", function()
    local conn = test_conn or setup_database()

    -- Clear existing data
    conn:execute("DELETE FROM users")

    -- Test batch INSERT using multiple VALUES
    local batch_insert_sql = [[
        INSERT INTO users (name, email, age, department, salary, active) VALUES
        ('Alice Johnson', 'alice@example.com', 28, 'Engineering', 75000.00, TRUE),
        ('Bob Smith', 'bob@example.com', 35, 'Marketing', 65000.00, TRUE),
        ('Charlie Brown', 'charlie@example.com', 42, 'Sales', 55000.00, FALSE),
        ('Diana Prince', 'diana@example.com', 31, 'Engineering', 85000.00, TRUE),
        ('Eve Wilson', 'eve@example.com', 29, 'HR', 60000.00, TRUE)
    ]]

    local result = conn:execute(batch_insert_sql)
    TestFramework.assert_not_nil(result)

    -- Verify batch insert count
    local count_result = conn:execute("SELECT COUNT(*) as count FROM users")
    TestFramework.assert_not_nil(count_result)

    if type(count_result) == "userdata" then
        local row = count_result:fetch()
        TestFramework.assert_equal(row.count, 5)
        count_result:close()
    end

    -- Test individual batch INSERT (simulating loop insertion)
    for i, user in ipairs(TEST_USERS) do
        if i <= 3 then -- Only insert first 3 to avoid duplicates
            local individual_insert = string.format([[
                INSERT INTO users (name, email, age, department, salary, active)
                VALUES ('%s', '%s_batch', %d, '%s', %.2f, %s)
            ]],
                user.name .. " Batch", user.email:gsub("@", "_batch@"),
                user.age, user.department, user.salary,
                user.active and "TRUE" or "FALSE"
            )

            local individual_result = conn:execute(individual_insert)
            TestFramework.assert_not_nil(individual_result)
        end
    end

    -- Verify total count after individual inserts
    local final_count_result = conn:execute("SELECT COUNT(*) as count FROM users")
    if type(final_count_result) == "userdata" then
        local row = final_count_result:fetch()
        TestFramework.assert_equal(row.count, 8) -- 5 batch + 3 individual
        final_count_result:close()
    end

    print("✓ Batch INSERT operations test passed")
end)

-- Test 3: Basic SELECT operations
suite:test("basic_select_operations", function()
    local conn = test_conn or setup_database()

    -- Test SELECT all
    local all_result = conn:execute("SELECT * FROM users")
    TestFramework.assert_not_nil(all_result)

    if type(all_result) == "userdata" then
        local count = 0
        local row = all_result:fetch()
        while row do
            count = count + 1
            TestFramework.assert_not_nil(row.id)
            TestFramework.assert_not_nil(row.name)
            TestFramework.assert_not_nil(row.email)
            row = all_result:fetch()
        end
        TestFramework.assert_true(count > 0)
        all_result:close()
    end

    -- Test SELECT with WHERE condition
    local filtered_result = conn:execute("SELECT * FROM users WHERE department = 'Engineering'")
    TestFramework.assert_not_nil(filtered_result)

    if type(filtered_result) == "userdata" then
        local eng_count = 0
        local row = filtered_result:fetch()
        while row do
            eng_count = eng_count + 1
            TestFramework.assert_equal(row.department, "Engineering")
            row = filtered_result:fetch()
        end
        TestFramework.assert_true(eng_count > 0)
        filtered_result:close()
    end

    -- Test SELECT with ORDER BY
    local ordered_result = conn:execute("SELECT name, age FROM users ORDER BY age DESC LIMIT 3")
    TestFramework.assert_not_nil(ordered_result)

    if type(ordered_result) == "userdata" then
        local prev_age = math.huge
        local row = ordered_result:fetch()
        while row do
            TestFramework.assert_true(row.age <= prev_age)
            prev_age = row.age
            row = ordered_result:fetch()
        end
        ordered_result:close()
    end

    print("✓ Basic SELECT operations test passed")
end)

-- Test 4: Aggregate functions
suite:test("aggregate_functions", function()
    local conn = test_conn or setup_database()

    -- Test COUNT
    local count_result = conn:execute("SELECT COUNT(*) as total_users FROM users")
    TestFramework.assert_not_nil(count_result)

    if type(count_result) == "userdata" then
        local row = count_result:fetch()
        TestFramework.assert_not_nil(row.total_users)
        TestFramework.assert_true(row.total_users > 0)
        count_result:close()
    end

    -- Test AVG, MIN, MAX
    local stats_result = conn:execute([[
        SELECT
            AVG(salary) as avg_salary,
            MIN(salary) as min_salary,
            MAX(salary) as max_salary,
            COUNT(DISTINCT department) as dept_count
        FROM users
        WHERE salary IS NOT NULL
    ]])
    TestFramework.assert_not_nil(stats_result)

    if type(stats_result) == "userdata" then
        local row = stats_result:fetch()
        TestFramework.assert_not_nil(row.avg_salary)
        TestFramework.assert_not_nil(row.min_salary)
        TestFramework.assert_not_nil(row.max_salary)
        TestFramework.assert_true(row.avg_salary >= row.min_salary)
        TestFramework.assert_true(row.avg_salary <= row.max_salary)
        TestFramework.assert_true(row.dept_count > 0)
        stats_result:close()
    end

    -- Test GROUP BY
    local group_result = conn:execute([[
        SELECT department, COUNT(*) as dept_count, AVG(salary) as dept_avg_salary
        FROM users
        WHERE department IS NOT NULL
        GROUP BY department
        ORDER BY dept_count DESC
    ]])
    TestFramework.assert_not_nil(group_result)

    if type(group_result) == "userdata" then
        local row = group_result:fetch()
        while row do
            TestFramework.assert_not_nil(row.department)
            TestFramework.assert_true(row.dept_count > 0)
            TestFramework.assert_not_nil(row.dept_avg_salary)
            row = group_result:fetch()
        end
        group_result:close()
    end

    print("✓ Aggregate functions test passed")
end)

-- Test 5: UPDATE operations
suite:test("update_operations", function()
    local conn = test_conn or setup_database()

    -- Test single UPDATE
    local update_result = conn:execute([[
        UPDATE users
        SET salary = 80000.00, department = 'Senior Engineering'
        WHERE name = 'Alice Johnson'
    ]])
    TestFramework.assert_not_nil(update_result)

    -- Verify the update
    local verify_result = conn:execute("SELECT salary, department FROM users WHERE name = 'Alice Johnson'")
    TestFramework.assert_not_nil(verify_result)

    if type(verify_result) == "userdata" then
        local row = verify_result:fetch()
        TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.salary, 80000.00)
        TestFramework.assert_equal(row.department, "Senior Engineering")
        verify_result:close()
    end

    -- Test batch UPDATE with conditions
    local batch_update_result = conn:execute([[
        UPDATE users
        SET salary = salary * 1.10
        WHERE department IN ('Engineering', 'Senior Engineering') AND active = TRUE
    ]])
    TestFramework.assert_not_nil(batch_update_result)

    -- Test conditional UPDATE
    local conditional_update = conn:execute([[
        UPDATE users
        SET active = FALSE
        WHERE age > 40
    ]])
    TestFramework.assert_not_nil(conditional_update)

    -- Verify conditional update
    local inactive_count_result = conn:execute("SELECT COUNT(*) as count FROM users WHERE active = FALSE")
    if type(inactive_count_result) == "userdata" then
        local row = inactive_count_result:fetch()
        TestFramework.assert_true(row.count > 0)
        inactive_count_result:close()
    end

    print("✓ UPDATE operations test passed")
end)

-- Test 6: DELETE operations
suite:test("delete_operations", function()
    local conn = test_conn or setup_database()

    -- Get initial count
    local initial_count_result = conn:execute("SELECT COUNT(*) as count FROM users")
    local initial_count = 0
    if type(initial_count_result) == "userdata" then
        local row = initial_count_result:fetch()
        initial_count = row.count
        initial_count_result:close()
    end

    -- Test single DELETE
    local delete_result = conn:execute("DELETE FROM users WHERE email LIKE '%_batch@%' LIMIT 1")
    TestFramework.assert_not_nil(delete_result)

    -- Verify deletion
    local after_single_delete = conn:execute("SELECT COUNT(*) as count FROM users")
    if type(after_single_delete) == "userdata" then
        local row = after_single_delete:fetch()
        TestFramework.assert_equal(row.count, initial_count - 1)
        after_single_delete:close()
    end

    -- Test conditional DELETE
    local conditional_delete = conn:execute("DELETE FROM users WHERE active = FALSE")
    TestFramework.assert_not_nil(conditional_delete)

    -- Verify conditional deletion
    local active_only_result = conn:execute("SELECT COUNT(*) as count FROM users WHERE active = FALSE")
    if type(active_only_result) == "userdata" then
        local row = active_only_result:fetch()
        TestFramework.assert_equal(row.count, 0)
        active_only_result:close()
    end

    -- Test DELETE with complex WHERE clause
    local complex_delete = conn:execute([[
        DELETE FROM users
        WHERE department = 'Marketing' AND salary < 70000
    ]])
    TestFramework.assert_not_nil(complex_delete)

    print("✓ DELETE operations test passed")
end)

-- Test 7: JOIN operations (with additional table)
suite:test("join_operations", function()
    local conn = test_conn or setup_database()

    -- Create orders table for JOIN testing
    local create_orders_sql = [[
        CREATE TABLE orders (
            id INT AUTO_INCREMENT PRIMARY KEY,
            user_id INT,
            product_name VARCHAR(100),
            quantity INT,
            price DECIMAL(10,2),
            order_date DATE,
            FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
        )
    ]]

    local create_result = conn:execute(create_orders_sql)
    TestFramework.assert_not_nil(create_result)

    -- Get existing user IDs dynamically to avoid foreign key constraint errors
    local user_ids = {}
    local user_id_result = conn:execute("SELECT id FROM users ORDER BY id LIMIT 4")
    TestFramework.assert_not_nil(user_id_result)

    if type(user_id_result) == "userdata" then
        local row = user_id_result:fetch()
        while row do
            table.insert(user_ids, row.id)
            row = user_id_result:fetch()
        end
        user_id_result:close()
    end

    -- Ensure we have at least 3 users for the JOIN test
    if #user_ids < 3 then
        error("Not enough users found for JOIN test. Expected at least 3, found " .. #user_ids)
    end

    -- Insert some orders using actual user IDs
    local orders_insert = string.format([[
        INSERT INTO orders (user_id, product_name, quantity, price, order_date) VALUES
        (%d, 'Laptop', 1, 1200.00, '2023-01-15'),
        (%d, 'Mouse', 2, 25.00, '2023-01-15'),
        (%d, 'Keyboard', 1, 80.00, '2023-01-20'),
        (%d, 'Monitor', 1, 300.00, '2023-02-01')
    ]], user_ids[1], user_ids[1], user_ids[2], user_ids[3])

    local orders_result = conn:execute(orders_insert)
    TestFramework.assert_not_nil(orders_result)

    -- Test INNER JOIN
    local join_result = conn:execute([[
        SELECT u.name, u.email, o.product_name, o.quantity, o.price
        FROM users u
        INNER JOIN orders o ON u.id = o.user_id
        ORDER BY u.name, o.order_date
    ]])
    TestFramework.assert_not_nil(join_result)

    if type(join_result) == "userdata" then
        local join_count = 0
        local row = join_result:fetch()
        while row do
            join_count = join_count + 1
            TestFramework.assert_not_nil(row.name)
            TestFramework.assert_not_nil(row.product_name)
            TestFramework.assert_true(row.quantity > 0)
            TestFramework.assert_true(row.price > 0)
            row = join_result:fetch()
        end
        TestFramework.assert_true(join_count > 0)
        join_result:close()
    end

    -- Test LEFT JOIN
    local left_join_result = conn:execute([[
        SELECT u.name, COUNT(o.id) as order_count, COALESCE(SUM(o.price * o.quantity), 0) as total_spent
        FROM users u
        LEFT JOIN orders o ON u.id = o.user_id
        GROUP BY u.id, u.name
        ORDER BY total_spent DESC
    ]])
    TestFramework.assert_not_nil(left_join_result)

    if type(left_join_result) == "userdata" then
        local row = left_join_result:fetch()
        while row do
            TestFramework.assert_not_nil(row.name)
            TestFramework.assert_true(row.order_count >= 0)
            TestFramework.assert_true(row.total_spent >= 0)
            row = left_join_result:fetch()
        end
        left_join_result:close()
    end

    print("✓ JOIN operations test passed")
end)

-- Test 8: Complex queries with subqueries
suite:test("complex_queries_subqueries", function()
    local conn = test_conn or setup_database()

    -- Test subquery in WHERE clause
    local subquery_result = conn:execute([[
        SELECT name, salary, department
        FROM users
        WHERE salary > (SELECT AVG(salary) FROM users WHERE salary IS NOT NULL)
        ORDER BY salary DESC
    ]])
    TestFramework.assert_not_nil(subquery_result)

    if type(subquery_result) == "userdata" then
        local row = subquery_result:fetch()
        while row do
            TestFramework.assert_not_nil(row.name)
            TestFramework.assert_not_nil(row.salary)
            row = subquery_result:fetch()
        end
        subquery_result:close()
    end

    -- Test EXISTS subquery
    local exists_result = conn:execute([[
        SELECT u.name, u.department
        FROM users u
        WHERE EXISTS (
            SELECT 1 FROM orders o WHERE o.user_id = u.id
        )
    ]])
    TestFramework.assert_not_nil(exists_result)

    if type(exists_result) == "userdata" then
        local row = exists_result:fetch()
        while row do
            TestFramework.assert_not_nil(row.name)
            row = exists_result:fetch()
        end
        exists_result:close()
    end

    print("✓ Complex queries with subqueries test passed")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB Phase 2B tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 2B tests completed successfully!")