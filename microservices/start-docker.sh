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
docker-compose up --build -d

# Wait for services to be ready
echo "⏳ Waiting for services to be ready..."
sleep 15

# Check service health
echo "🔍 Checking service health..."
docker-compose ps

echo ""
echo "✅ Dockerized microservices are running!"
echo ""
echo "🌐 Access points:"
echo "  📊 Web Interface: http://localhost:8000"
echo "  📚 API Documentation: http://localhost:8000/docs"
echo "  📡 Sender Service: http://localhost:8081"
echo "  📥 Receiver Service: http://localhost:8082"
echo ""
echo "📋 Useful commands:"
echo "  View logs: docker-compose logs -f"
echo "  Stop services: docker-compose down"
echo "  Restart services: docker-compose restart"
echo "  View status: docker-compose ps"
echo ""

# Show logs for a few seconds
echo "📋 Recent logs:"
docker-compose logs --tail=20
