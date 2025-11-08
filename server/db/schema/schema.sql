-- Order Book Database Schema
-- PostgreSQL 12+

-- Drop tables if they exist (in reverse dependency order)
DROP TABLE IF EXISTS ask_levels;
DROP TABLE IF EXISTS bid_levels;
DROP TABLE IF EXISTS order_book_snapshots;
DROP TABLE IF EXISTS processing_sessions;

-- Processing sessions table
-- Tracks each DBN file upload and processing session
CREATE TABLE processing_sessions (
    session_id VARCHAR(100) PRIMARY KEY,
    symbol VARCHAR(100) NOT NULL,
    file_name VARCHAR(255) NOT NULL,
    file_size BIGINT NOT NULL,
    status VARCHAR(50) NOT NULL DEFAULT 'processing',
    error_message TEXT,
    
    -- Statistics
    messages_received BIGINT,
    orders_processed BIGINT,
    snapshots_written BIGINT,
    throughput DOUBLE PRECISION,
    avg_process_ns BIGINT,
    p99_process_ns BIGINT,
    
    -- Final book state
    final_total_orders BIGINT,
    final_bid_levels INT,
    final_ask_levels INT,
    final_best_bid DOUBLE PRECISION,
    final_best_ask DOUBLE PRECISION,
    final_spread DOUBLE PRECISION,
    
    -- Timestamps
    start_time TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    end_time TIMESTAMP WITH TIME ZONE
);

-- Index for faster lookup of sessions by symbol and status
CREATE INDEX idx_sessions_symbol ON processing_sessions (symbol, status, start_time DESC);

-- Order book snapshots table
-- Stores the order book state at each MBO message
CREATE TABLE order_book_snapshots (
    id BIGSERIAL PRIMARY KEY,
    session_id VARCHAR(100) NOT NULL REFERENCES processing_sessions(session_id) ON DELETE CASCADE,
    symbol VARCHAR(100) NOT NULL,
    timestamp_ns BIGINT NOT NULL,
    
    -- Best bid/ask (BBO)
    best_bid_price BIGINT,
    best_bid_size INT,
    best_bid_count INT,
    best_ask_price BIGINT,
    best_ask_size INT,
    best_ask_count INT,
    
    -- Statistics
    total_orders BIGINT,
    bid_level_count INT,
    ask_level_count INT,
    
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Indexes for faster lookups
CREATE INDEX idx_snapshots_symbol_ts ON order_book_snapshots (symbol, timestamp_ns);
CREATE INDEX idx_snapshots_session ON order_book_snapshots (session_id, timestamp_ns);

-- Bid levels table
-- Stores the top N bid levels for each snapshot
CREATE TABLE bid_levels (
    id BIGSERIAL PRIMARY KEY,
    snapshot_id BIGINT NOT NULL REFERENCES order_book_snapshots(id) ON DELETE CASCADE,
    price BIGINT NOT NULL,
    size INT NOT NULL,
    count INT NOT NULL,
    level_index INT NOT NULL
);

-- Index for faster lookup of bid levels by snapshot
CREATE INDEX idx_bid_levels_snapshot_id ON bid_levels (snapshot_id, level_index);

-- Ask levels table
-- Stores the top N ask levels for each snapshot
CREATE TABLE ask_levels (
    id BIGSERIAL PRIMARY KEY,
    snapshot_id BIGINT NOT NULL REFERENCES order_book_snapshots(id) ON DELETE CASCADE,
    price BIGINT NOT NULL,
    size INT NOT NULL,
    count INT NOT NULL,
    level_index INT NOT NULL
);

-- Index for faster lookup of ask levels by snapshot
CREATE INDEX idx_ask_levels_snapshot_id ON ask_levels (snapshot_id, level_index);

