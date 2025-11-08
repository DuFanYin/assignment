# Database Setup

This directory contains database schema and setup scripts for the Order Book PostgreSQL database.

## Prerequisites

- PostgreSQL 12 or later installed and running
- User with CREATE DATABASE privileges

## Setup

### 1. Create the Database

```bash
psql -U postgres -c "CREATE DATABASE orderbook;"
```

### 2. Run the Schema

```bash
psql -U postgres -d orderbook -f db/schema/schema.sql
```

### 3. Configure Connection

Update `config/config.ini` with your PostgreSQL connection details:

```ini
postgres.host=localhost
postgres.port=5432
postgres.dbname=orderbook
postgres.user=postgres
postgres.password=your_password
postgres.max_connections=10
postgres.connection_timeout=30
```

## Schema Overview

### Tables

- **processing_sessions**: Tracks each DBN file upload/processing session
- **order_book_snapshots**: Stores order book state at each MBO message
- **bid_levels**: Top N bid price levels for each snapshot
- **ask_levels**: Top N ask price levels for each snapshot

### Key Features

- Foreign key constraints ensure referential integrity
- CASCADE deletion automatically cleans up related records
- Indexes optimize queries by symbol, timestamp, and session
- Tracks processing statistics and final book state

## Querying Data

### Get latest session for a symbol
```sql
SELECT * FROM processing_sessions 
WHERE symbol = 'ESH4' AND status = 'completed' 
ORDER BY start_time DESC LIMIT 1;
```

### Get snapshots for a session
```sql
SELECT * FROM order_book_snapshots 
WHERE session_id = 'your_session_id' 
ORDER BY timestamp_ns ASC;
```

### Get bid/ask levels for a snapshot
```sql
SELECT * FROM bid_levels WHERE snapshot_id = 123 ORDER BY level_index;
SELECT * FROM ask_levels WHERE snapshot_id = 123 ORDER BY level_index;
```

