# C++ Sender Microservice - Cross-Platform Compatible
FROM ubuntu:22.04

# Set working directory
WORKDIR /app

# Install system dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    libssl-dev \
    zlib1g-dev \
    libcurl4-openssl-dev \
    libzstd-dev \
    curl \
    wget \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

# Install newer CMake (3.24+ required by databento-cpp)
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null && \
    echo 'deb https://apt.kitware.com/ubuntu/ jammy main' | tee /etc/apt/sources.list.d/kitware.list >/dev/null && \
    apt-get update && \
    apt-get install -y cmake && \
    rm -rf /var/lib/apt/lists/*

# Install databento-cpp from source
RUN git clone https://github.com/databento/databento-cpp.git && \
    cd databento-cpp && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Debug && \
    make -j$(nproc)

# Copy source files
COPY sender_microservice.cpp .
COPY src/ ./src/
COPY include/ ./include/
COPY CMakeLists.txt .

# Build the application
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Create data directory for shared volume
RUN mkdir -p /app/data

# Expose port
EXPOSE 8081

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8081/status || exit 1

# Run the application
CMD ["./build/sender_microservice"]
