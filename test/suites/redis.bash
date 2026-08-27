SUITE_redis_PROBE() {
    if ! $CCACHE --version | grep -Fq redis-storage &> /dev/null; then
        echo "redis-storage not available"
        return
    fi
    if ! command -v redis-server &> /dev/null; then
        echo "redis-server not found"
        return
    fi
    if ! command -v redis-cli &> /dev/null; then
        echo "redis-cli not found"
        return
    fi
}

pick_redis_port() {
    # Pick a pseudo-random port to reduce the risk of colliding with a stale
    # redis-server left over from a previous (failed) run.
    echo $((20000 + ($$ + RANDOM) % 20000))
}

start_redis_server() {
    local port="$1"
    local password="${2:-}"

    redis-server --bind localhost --port "${port}" >/dev/null &
    REDIS_SERVER_PID=$!
    # Wait for server start.
    i=0
    while [ $i -lt 100 ] && ! redis-cli -p "${port}" ping &>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done

    # Make sure we start from a known state even if the port was inherited
    # from a stale server.
    redis-cli -p "${port}" flushall &>/dev/null

    if [ -n "${password}" ]; then
        redis-cli -p "${port}" config set requirepass "${password}" &>/dev/null
    fi
}

stop_redis_server() {
    if [ -n "${REDIS_SERVER_PID}" ]; then
        kill "${REDIS_SERVER_PID}" 2>/dev/null
        wait "${REDIS_SERVER_PID}" 2>/dev/null
        REDIS_SERVER_PID=
    fi
}

SUITE_redis_SETUP() {
    unset CCACHE_NODIRECT

    generate_code 1 test.c
}

expect_number_of_redis_cache_entries() {
    local expected=$1
    local port=$2
    local password="${3:-}"
    local actual

    if [ -n "${password}" ]; then
        actual=$(redis-cli -p "${port}" -a "${password}" \
                 keys "ccache:*" 2>/dev/null | wc -l)
    else
        actual=$(redis-cli -p "${port}" keys "ccache:*" 2>/dev/null | wc -l)
    fi
    if [ "$actual" -ne "$expected" ]; then
        test_failed "Found $actual (expected $expected) entries in Redis"
    fi
}

redis_tests() {
    # Don't leave a redis-server behind if a test fails.
    trap stop_redis_server EXIT

    # -------------------------------------------------------------------------
    TEST "Base case"

    port=$(pick_redis_port)
    export CCACHE_REMOTE_STORAGE="redis://localhost:${port}"

    start_redis_server "${port}"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2
    expect_stat 'remote storage miss' 1
    expect_number_of_redis_cache_entries 2 "${port}" # entry + manifest

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 1
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2
    expect_number_of_redis_cache_entries 2 "${port}" # entry + manifest

    $CCACHE -C >/dev/null
    expect_stat 'files in cache' 0
    expect_number_of_redis_cache_entries 2 "${port}" # -C does not touch remote

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 2
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2 # fetched from remote storage
    expect_stat 'remote storage hit' 1
    expect_number_of_redis_cache_entries 2 "${port}" # entry + manifest

    stop_redis_server

    # -------------------------------------------------------------------------
    TEST "Password"

    port=$(pick_redis_port)
    password=secret123
    export CCACHE_REMOTE_STORAGE="redis://${password}@localhost:${port}"

    start_redis_server "${port}" "${password}"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2
    expect_number_of_redis_cache_entries 2 "${port}" "${password}"

    # The password must not be written to the log.
    if grep -q "${password}" "$CCACHE_LOGFILE" 2>/dev/null; then
        test_failed "Password leaked to $CCACHE_LOGFILE"
    fi

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 1
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2
    expect_number_of_redis_cache_entries 2 "${port}" "${password}"

    $CCACHE -C >/dev/null
    expect_stat 'files in cache' 0
    expect_number_of_redis_cache_entries 2 "${port}" "${password}"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 2
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2 # fetched from remote storage
    expect_stat 'remote storage hit' 1
    expect_number_of_redis_cache_entries 2 "${port}" "${password}"

    stop_redis_server

    # -------------------------------------------------------------------------
    TEST "Authentication failure"

    port=$(pick_redis_port)
    export CCACHE_REMOTE_STORAGE="redis://wrongpassword@localhost:${port}"

    start_redis_server "${port}" "realpassword"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2 # local caching still works
    expect_stat 'remote storage errors' 1

    stop_redis_server

    # -------------------------------------------------------------------------
    TEST "Remote only"

    port=$(pick_redis_port)
    export CCACHE_REMOTE_STORAGE="redis://localhost:${port}"
    export CCACHE_REMOTE_ONLY=1

    start_redis_server "${port}"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 0 # only kept in remote storage
    expect_stat 'remote storage miss' 1
    expect_number_of_redis_cache_entries 2 "${port}" # entry + manifest

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 1
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 0 # nothing kept locally
    expect_stat 'remote storage hit' 1
    expect_number_of_redis_cache_entries 2 "${port}"

    unset CCACHE_REMOTE_ONLY
    stop_redis_server

    # -------------------------------------------------------------------------
    TEST "Compression and entry content"

    port=$(pick_redis_port)
    export CCACHE_REMOTE_STORAGE="redis://localhost:${port}"
    export CCACHE_COMPRESS=1

    start_redis_server "${port}"

    $REAL_COMPILER -c -o reference_test.o test.c

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache miss' 1
    expect_number_of_redis_cache_entries 2 "${port}"

    $CCACHE -C >/dev/null

    # The entry fetched back from remote storage must produce an identical
    # object file.
    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 1
    expect_stat 'remote storage hit' 1
    expect_equal_object_files reference_test.o test.o

    unset CCACHE_COMPRESS
    stop_redis_server

    # -------------------------------------------------------------------------
    TEST "Unreachable server"

    export CCACHE_REMOTE_STORAGE="redis://localhost:1"

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache miss' 1
    expect_stat 'files in cache' 2
    expect_stat 'remote storage errors' 1
}

SUITE_redis() {
    redis_tests
}
