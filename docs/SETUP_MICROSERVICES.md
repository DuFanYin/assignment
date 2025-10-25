# Setup Guide - Microservices Version (`/microservices/`)

> **💡 TIP:** Use the provided `start-docker.sh` script for the easiest setup! See [Quick Start](#quick-start-2-steps) below.

> **📖 For architecture overview and output format:** See main [README.md](../README.md)

## Prerequisites

### System Requirements
- **OS**: Linux, macOS, or Windows with WSL2
- **Docker**: Version 20.10 or higher
- **Docker Compose**: Version 2.0 or higher
- **Memory**: At least 4GB RAM allocated to Docker
- **Disk**: 2GB free space

### Required Tools

**macOS:**
```bash
# Install Docker Desktop
brew install --cask docker

# Start Docker Desktop application
open -a Docker
```

**Ubuntu/Debian:**
```bash
# Install Docker
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh

# Install Docker Compose
sudo apt-get update
sudo apt-get install docker-compose-plugin

# Add user to docker group
sudo usermod -aG docker $USER
newgrp docker
```

**Verify Installation:**
```bash
docker --version
docker compose version
```

## Quick Start (2 Steps)

### Step 1: Place the Data File

Ensure the DBN market data file is in the correct location:

```
📁 /Users/hang/github_repo/assignment/microservices/shared-data/CLX5_mbo.dbn
```

**If the file is missing, copy it from:**
- `/src/data/CLX5_mbo.dbn` 
- OR `/server/data/CLX5_mbo.dbn`

### Step 2: Run the Start Script

```bash
cd /Users/hang/github_repo/assignment/microservices
./start-docker.sh
```

**That's it!** The script will:
1. Check Docker is running
2. Build all images
3. Start all services
4. Open http://localhost:8000 in your browser

Click **"Start Streaming"** button to process market data.

---

## What the Start Script Does

The `start-docker.sh` script handles everything automatically:

```bash
✅ Checks Docker is running
✅ Stops any existing containers
✅ Builds all Docker images (sender, receiver, python-server)
✅ Starts all microservices
✅ Shows service status and logs
✅ Displays access URLs
```

**Output you'll see:**
```
🐳 Starting Dockerized Order Book Microservices...
🔄 Stopping existing containers...
🏗️ Building and starting all microservices...
⏳ Waiting for services to be ready...
✅ Dockerized microservices are running!

🌐 Access points:
  📊 Web Interface: http://localhost:8000
  📡 Sender Service: http://localhost:8081
  📥 Receiver Service: http://localhost:8082
```

## Output Files

**Location (in container):** `/shared-data/order_book_output.json`

**Location (on host):** `/Users/hang/github_repo/assignment/microservices/shared-data/order_book_output.json`

**Format & Metrics:** See main [README.md](../README.md#-output-format--metrics)

## Troubleshooting

### Build Errors

**Error: `failed to solve: process "/bin/sh -c apt-get update" did not complete successfully`**
```bash
# Solution: Check internet connection and retry
docker compose build --no-cache
```

**Error: `ERROR: Cannot locate specified Dockerfile: Dockerfile`**
```bash
# Solution: Ensure you're in the microservices directory
cd /Users/hang/github_repo/assignment/microservices
ls */Dockerfile  # Should show Dockerfiles in each service directory
```

### Runtime Errors

**Error: `Error response from daemon: Ports are not available`**
```bash
# Solution: Kill processes using ports 8000, 8081, 8082
lsof -ti:8000,8081,8082 | xargs kill -9

# Then restart
docker compose up
```

**Error: `Cannot start service: driver failed programming external connectivity`**
```bash
# Solution: Restart Docker
# macOS: Restart Docker Desktop
# Linux:
sudo systemctl restart docker
docker compose up
```

**Error: `no such file or directory: /shared-data/CLX5_mbo.dbn`**
```bash
# Solution: Ensure DBN file exists in shared-data
ls microservices/shared-data/CLX5_mbo.dbn

# If missing, copy it
cp server/data/CLX5_mbo.dbn microservices/shared-data/
```

### Container Issues

**Issue: Container exits immediately**
```bash
# Check container logs
docker compose logs sender-microservice

# Check container status
docker compose ps -a

# Inspect container
docker compose exec sender-microservice /bin/bash
```

**Issue: Cannot connect to microservices**
```bash
# Check if containers are running
docker compose ps

# Check network connectivity
docker compose exec python-server ping sender-microservice
docker compose exec python-server ping receiver-microservice

# Restart services
docker compose restart
```

**Issue: High memory usage**
```bash
# Check resource usage
docker stats

# Limit resources in docker-compose.yml
services:
  receiver-microservice:
    deploy:
      resources:
        limits:
          memory: 2G
```

### Data Issues

**Issue: Order book file not created**
```bash
# Check shared volume
docker compose exec receiver-microservice ls -la /shared-data/

# Check permissions
docker compose exec receiver-microservice ls -la /shared-data/order_book_output.json

# Check disk space
df -h
```

**Issue: Missing records in output**
- Ensure you're using the latest code with JSON flushing fixes
- Check receiver logs for errors
- Verify the DBN input file is complete

## Next Steps

- **Try standalone version**: See [SETUP_SRC.md](./SETUP_SRC.md)
- **Try server version**: See [SETUP_SERVER.md](./SETUP_SERVER.md)
- **View architecture & metrics**: See main [README.md](../README.md)

## Support

For issues or questions:
1. Check the troubleshooting section above
2. Review Docker logs: `docker compose logs`
3. Check Docker status: `docker compose ps`
4. Examine the Dockerfiles in each service directory
5. Review the main [README.md](../README.md)
