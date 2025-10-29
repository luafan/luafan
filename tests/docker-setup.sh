#!/bin/bash

# LuaFan MariaDB Docker Environment Setup Script
# Manages the MariaDB container for testing

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

CONTAINER_NAME="luafan-test-mariadb"
COMPOSE_FILE="docker-compose.yml"

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if container is running
is_container_running() {
    docker ps -q -f name=$CONTAINER_NAME | grep -q .
}

# Function to check if container exists (running or stopped)
container_exists() {
    docker ps -a -q -f name=$CONTAINER_NAME | grep -q .
}

# Function to wait for MariaDB to be ready
wait_for_mariadb() {
    print_status "Waiting for MariaDB to be ready..."
    local max_attempts=30
    local attempt=1

    while [ $attempt -le $max_attempts ]; do
        if docker exec $CONTAINER_NAME mysql -utest_user -ptest_password -e "SELECT 1;" test_db >/dev/null 2>&1; then
            print_success "MariaDB is ready for connections!"
            return 0
        fi

        print_status "Attempt $attempt/$max_attempts - MariaDB not ready yet..."
        sleep 2
        attempt=$((attempt + 1))
    done

    print_error "MariaDB failed to become ready within timeout"
    return 1
}

# Function to test database connection
test_connection() {
    print_status "Testing database connection..."

    if docker exec $CONTAINER_NAME mysql -utest_user -ptest_password -e "
        SELECT 'Connection successful!' as status, NOW() as timestamp;
        SELECT COUNT(*) as init_records FROM connection_test;
    " test_db; then
        print_success "Database connection test passed!"
        return 0
    else
        print_error "Database connection test failed!"
        return 1
    fi
}

# Function to show container status
show_status() {
    print_status "MariaDB Container Status:"
    echo "----------------------------------------"

    if container_exists; then
        docker ps -a -f name=$CONTAINER_NAME --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
        echo ""

        if is_container_running; then
            print_status "Container logs (last 10 lines):"
            docker logs --tail 10 $CONTAINER_NAME
        fi
    else
        print_warning "Container does not exist"
    fi
}

# Function to start MariaDB
start_mariadb() {
    print_status "Starting MariaDB container..."

    if is_container_running; then
        print_warning "MariaDB container is already running"
        show_status
        return 0
    fi

    # Start container with docker compose
    docker compose -f $COMPOSE_FILE up -d

    # Wait for MariaDB to be ready
    if wait_for_mariadb; then
        test_connection
        print_success "MariaDB is now running and ready for tests!"
        echo ""
        print_status "Connection details:"
        echo "  Host: localhost"
        echo "  Port: 3306"
        echo "  Database: test_db"
        echo "  Username: test_user"
        echo "  Password: test_password"
    else
        print_error "Failed to start MariaDB properly"
        return 1
    fi
}

# Function to stop MariaDB
stop_mariadb() {
    print_status "Stopping MariaDB container..."

    if ! is_container_running; then
        print_warning "MariaDB container is not running"
        return 0
    fi

    docker compose -f $COMPOSE_FILE down
    print_success "MariaDB container stopped"
}

# Function to restart MariaDB
restart_mariadb() {
    print_status "Restarting MariaDB container..."
    stop_mariadb
    sleep 2
    start_mariadb
}

# Function to remove MariaDB container and volumes
clean_mariadb() {
    print_status "Cleaning up MariaDB container and data..."

    docker compose -f $COMPOSE_FILE down -v

    # Remove any orphaned volumes
    if docker volume ls -q -f name=tests_mariadb_data | grep -q .; then
        docker volume rm tests_mariadb_data
        print_status "Removed MariaDB data volume"
    fi

    print_success "MariaDB environment cleaned up"
}

# Function to show logs
show_logs() {
    if container_exists; then
        docker logs -f $CONTAINER_NAME
    else
        print_error "Container does not exist"
        return 1
    fi
}

# Function to run mysql shell
mysql_shell() {
    if is_container_running; then
        print_status "Opening MySQL shell (test_user@test_db)..."
        print_status "Type 'exit' to quit"
        docker exec -it $CONTAINER_NAME mysql -utest_user -ptest_password test_db
    else
        print_error "MariaDB container is not running. Start it first with: $0 start"
        return 1
    fi
}

# Function to show usage
show_usage() {
    echo "LuaFan MariaDB Docker Environment Manager"
    echo ""
    echo "Usage: $0 {start|stop|restart|status|clean|logs|shell|test}"
    echo ""
    echo "Commands:"
    echo "  start     - Start MariaDB container"
    echo "  stop      - Stop MariaDB container"
    echo "  restart   - Restart MariaDB container"
    echo "  status    - Show container status"
    echo "  clean     - Remove container and data volumes"
    echo "  logs      - Show container logs (follow mode)"
    echo "  shell     - Open MySQL shell"
    echo "  test      - Test database connection"
    echo ""
    echo "Examples:"
    echo "  $0 start          # Start MariaDB for testing"
    echo "  $0 test           # Test if database is working"
    echo "  $0 shell          # Open MySQL command line"
    echo "  $0 stop           # Stop MariaDB when done"
}

# Main script logic
case "${1:-}" in
    start)
        start_mariadb
        ;;
    stop)
        stop_mariadb
        ;;
    restart)
        restart_mariadb
        ;;
    status)
        show_status
        ;;
    clean)
        clean_mariadb
        ;;
    logs)
        show_logs
        ;;
    shell)
        mysql_shell
        ;;
    test)
        if is_container_running; then
            test_connection
        else
            print_error "MariaDB container is not running. Start it first with: $0 start"
            exit 1
        fi
        ;;
    *)
        show_usage
        exit 1
        ;;
esac