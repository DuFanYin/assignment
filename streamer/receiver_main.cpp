#include "tcp_receiver.hpp"
#include <iostream>
#include <signal.h>
#include <chrono>
#include <iomanip>
#include <sstream>

// Global receiver for signal handling
TCPReceiver* g_receiver = nullptr;

void signalHandler(int signal) {
    if (g_receiver) {
        std::cout << "\n🛑 Received signal " << signal << ", stopping..." << std::endl;
        g_receiver->disconnect();
    }
    exit(0);
}

int main() {
    std::cout << "=== TCP Message Receiver (Timing Test) ===" << std::endl;
    
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create receiver
    auto receiver = std::make_unique<TCPReceiver>();
    g_receiver = receiver.get();
    
    // Configure receiver
    receiver->setHost("127.0.0.1");
    receiver->setPort(8080);
    
    // Timing statistics
    double totalLatencyMs = 0.0;
    double minLatencyMs = std::numeric_limits<double>::max();
    double maxLatencyMs = 0.0;
    size_t messageCount = 0;
    
    // Set up message callback to measure latency
    receiver->setMessageCallback([&](const std::string& message) {
        // Parse message format: "MBO:order_id:price:size:action:side:timestamp"
        std::istringstream iss(message);
        std::string token;
        std::vector<std::string> tokens;
        
        while (std::getline(iss, token, ':')) {
            tokens.push_back(token);
        }
        
        if (tokens.size() >= 7 && tokens[0] == "MBO") {
            try {
                // Get current timestamp
                auto now = std::chrono::high_resolution_clock::now();
                auto currentTimestamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                
                // Parse sent timestamp
                auto sentTimestamp = std::stoull(tokens[6]);
                
                // Calculate latency in milliseconds
                double latencyMs = (currentTimestamp - sentTimestamp) / 1000.0;
                
                // Update statistics
                totalLatencyMs += latencyMs;
                minLatencyMs = std::min(minLatencyMs, latencyMs);
                maxLatencyMs = std::max(maxLatencyMs, latencyMs);
                messageCount++;
                
                // No per-message printing - only final results
                
            } catch (const std::exception& e) {
                std::cerr << "❌ Error parsing message: " << message << " - " << e.what() << std::endl;
            }
        }
    });
    
    // Connect to sender
    if (!receiver->connect()) {
        std::cerr << "❌ Failed to connect to sender" << std::endl;
        return 1;
    }
    
    // Keep running until disconnected
    std::cout << "🚀 TCP receiver running... Press Ctrl+C to stop" << std::endl;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    while (receiver->isConnected()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Final statistics
    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    std::cout << "\n=== Final Timing Statistics ===" << std::endl;
    std::cout << "Total Time: " << totalDuration.count() << " ms" << std::endl;
    std::cout << "Total Messages: " << messageCount << std::endl;
    
    if (messageCount > 0) {
        double avgLatencyMs = totalLatencyMs / messageCount;
        double messagesPerSecond = (double)messageCount * 1000.0 / totalDuration.count();
        
        std::cout << "Average Latency: " << std::fixed << std::setprecision(3) << avgLatencyMs << " ms" << std::endl;
        std::cout << "Min Latency: " << std::fixed << std::setprecision(3) << minLatencyMs << " ms" << std::endl;
        std::cout << "Max Latency: " << std::fixed << std::setprecision(3) << maxLatencyMs << " ms" << std::endl;
        std::cout << "Messages/sec: " << std::fixed << std::setprecision(0) << messagesPerSecond << std::endl;
    }
    
    std::cout << "========================" << std::endl;
    
    std::cout << "✅ TCP receiver stopped" << std::endl;
    return 0;
}
