-- ClickHouse Schema for Order Book Processing
-- Ultra-fast columnar storage optimized for time-series data

-- Create database if it doesn't exist
CREATE DATABASE IF NOT EXISTS orderbook;

USE orderbook;

-- Processing sessions table
CREATE TABLE IF NOT EXISTS processing_sessions (
    session_id String,
    symbol String,
    file_name String,
    file_size UInt64,
    status String,
    start_time DateTime DEFAULT now(),
    end_time Nullable(DateTime),
    messages_received UInt64 DEFAULT 0,
    orders_processed UInt64 DEFAULT 0,
    snapshots_written UInt64 DEFAULT 0,
    throughput Float64 DEFAULT 0,
    avg_process_ns Int64 DEFAULT 0,
    p99_process_ns UInt64 DEFAULT 0,
    final_total_orders UInt64 DEFAULT 0,
    final_bid_levels UInt64 DEFAULT 0,
    final_ask_levels UInt64 DEFAULT 0,
    final_best_bid Float64 DEFAULT 0,
    final_best_ask Float64 DEFAULT 0,
    final_spread Float64 DEFAULT 0,
    error_message Nullable(String)
) ENGINE = MergeTree()
ORDER BY (session_id, start_time)
SETTINGS index_granularity = 8192;

-- Order book snapshots table with native arrays (MUCH faster than JSONB!)
CREATE TABLE IF NOT EXISTS order_book_snapshots (
    id UInt64,
    session_id String,
    symbol String,
    timestamp_ns Int64,
    best_bid_price Float64,
    best_bid_size UInt32,
    best_bid_count UInt32,
    best_ask_price Float64,
    best_ask_size UInt32,
    best_ask_count UInt32,
    total_orders UInt64,
    bid_level_count UInt32,
    ask_level_count UInt32,
    -- Native arrays: [{"price":123,"size":10,"count":3},...]
    -- No JSON parsing overhead!
    -- Prices stored as Float64 with 2 decimal places
    bid_levels Array(Tuple(price Float64, size UInt32, count UInt32)),
    ask_levels Array(Tuple(price Float64, size UInt32, count UInt32))
) ENGINE = MergeTree()
ORDER BY (session_id, timestamp_ns)
PARTITION BY toYYYYMM(toDateTime(timestamp_ns / 1000000000))
SETTINGS index_granularity = 8192;

-- Indexes for faster queries
CREATE INDEX IF NOT EXISTS idx_snapshots_symbol ON order_book_snapshots (symbol) TYPE minmax GRANULARITY 4;

