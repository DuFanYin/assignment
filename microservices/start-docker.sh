#!/bin/bash

# Dockerized Order Book Microservices Startup Script

echo "🐳 Starting Dockerized Order Book Microservices..."

# Navigate to microservices directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
cd "$SCRIPT_DIR"

# Check if Docker is running
if ! docker info > /dev/null 2>&1; then
    echo "❌ Docker is not running. Please start Docker first."
    exit 1
fi

# Check if Docker Compose is available
if ! command -v docker-compose &> /dev/null; then
    echo "❌ Docker Compose is not installed. Please install Docker Compose first."
    exit 1
fi

# Stop any existing containers
echo "🔄 Stopping existing containers..."
docker-compose down

# Build and start all services
echo "🏗️ Building and starting all microservices..."
echo "📋 Starting with logs visible..."
docker-compose up --build
