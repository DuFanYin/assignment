#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <fstream>
#include "../include/tcp_sender.hpp"
#include "../include/utils.hpp"

class SenderMicroservice {
private:
    int serverSocket_;
    int port_;
    bool running_;
    std::unique_ptr<TCPSender> sender_;
    
public:
    SenderMicroservice(int port = 8081) : serverSocket_(-1), port_(port), running_(false) {
        sender_ = std::make_unique<TCPSender>();
    }
    
    ~SenderMicroservice() {
        stop();
    }
    
    bool start() {
        // Create socket
        serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket_ == -1) {
            std::cerr << "❌ Failed to create socket" << std::endl;
            return false;
        }
        
        // Set socket options
        int opt = 1;
        if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            std::cerr << "❌ Failed to set socket options" << std::endl;
            return false;
        }
        
        // Bind socket
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);
        
        if (bind(serverSocket_, (struct sockaddr*)&address, sizeof(address)) == -1) {
            std::cerr << "❌ Failed to bind socket to port " << port_ << std::endl;
            return false;
        }
        
        // Listen for connections
        if (listen(serverSocket_, 5) == -1) {
            std::cerr << "❌ Failed to listen on socket" << std::endl;
            return false;
        }
        
        running_ = true;
        std::cout << "🚀 Sender Microservice started on port " << port_ << std::endl;
        std::cout << "📡 Ready to receive streaming requests..." << std::endl;
        
        return true;
    }
    
    void run() {
        while (running_) {
            // Accept connection
            struct sockaddr_in clientAddress;
            socklen_t clientLen = sizeof(clientAddress);
            int clientSocket = accept(serverSocket_, (struct sockaddr*)&clientAddress, &clientLen);
            
            if (clientSocket == -1) {
                if (running_) {
                    std::cerr << "❌ Failed to accept connection" << std::endl;
                }
                continue;
            }
            
            // Handle request in a separate thread
            std::thread(&SenderMicroservice::handleRequest, this, clientSocket).detach();
        }
    }
    
    void handleRequest(int clientSocket) {
        char buffer[1024];
        ssize_t bytesRead = read(clientSocket, buffer, sizeof(buffer) - 1);
        
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::string request(buffer);
            
            // Simple HTTP request parsing
            if (request.find("POST /start-streaming") != std::string::npos) {
                handleStartStreaming(clientSocket);
            } else if (request.find("GET /status") != std::string::npos) {
                handleStatus(clientSocket);
            } else {
                handleNotFound(clientSocket);
            }
        }
        
        close(clientSocket);
    }
    
    void handleStartStreaming(int clientSocket) {
        std::cout << "📡 Received start streaming request" << std::endl;
        
        try {
            // Configure sender exactly like the original sender_main.cpp
            sender_->setDelayMs(0);  // No artificial delays
            sender_->setZeroCopyMode(false);  // Use message-by-message mode
            sender_->setPort(8080);
            
            // Load data file (relative path from server directory)
            std::string dataFile = "data/CLX5_mbo.dbn";
            if (!sender_->loadFromFile(dataFile)) {
                sendErrorResponse(clientSocket, "Failed to load data file: " + dataFile);
                return;
            }
            
            std::cout << "📁 Data File: " << dataFile << std::endl;
            std::cout << "🌐 Server Port: 8080" << std::endl;
            
            // Start streaming in a separate thread so it doesn't block HTTP response
            std::cout << "🚀 Starting TCP streaming in background..." << std::endl;
            std::thread streamingThread([this]() {
                sender_->startStreaming();
                
                // Wait for streaming to complete
                while (sender_->isStreaming()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                std::cout << "✅ TCP streaming completed!" << std::endl;
            });
            
            // Detach the thread so it runs independently
            streamingThread.detach();
            
            // Send immediate success response
            std::stringstream response;
            response << "{\"status\":\"success\",\"message\":\"Sender started, waiting for connection\","
                     << "\"port\":8080}";
            
            sendJsonResponse(clientSocket, response.str());
            
        } catch (const std::exception& e) {
            sendErrorResponse(clientSocket, std::string("Error: ") + e.what());
        }
    }
    
    void handleStatus(int clientSocket) {
        std::string response = "{\"status\":\"ready\",\"service\":\"sender\",\"port\":8080}";
        sendJsonResponse(clientSocket, response);
    }
    
    void handleNotFound(int clientSocket) {
        std::string response = "{\"error\":\"Not Found\"}";
        sendErrorResponse(clientSocket, response);
    }
    
    void sendJsonResponse(int clientSocket, const std::string& json) {
        std::string response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "\r\n" + json;
        
        std::cout << "📤 Sending HTTP response (length: " << response.length() << "):" << std::endl;
        std::cout << response.substr(0, 200) << "..." << std::endl;
        
        ssize_t sent = send(clientSocket, response.c_str(), response.length(), 0);
        if (sent == static_cast<ssize_t>(response.length())) {
            std::cout << "✅ HTTP response sent successfully (" << sent << " bytes)" << std::endl;
        } else {
            std::cout << "❌ Failed to send HTTP response (sent " << sent << " of " << response.length() << " bytes)" << std::endl;
        }
    }
    
    void sendErrorResponse(int clientSocket, const std::string& error) {
        std::string json = "{\"status\":\"error\",\"message\":\"" + error + "\"}";
        std::string response = "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "\r\n" + json;
        send(clientSocket, response.c_str(), response.length(), 0);
    }
    
    void stop() {
        running_ = false;
        if (serverSocket_ != -1) {
            close(serverSocket_);
            serverSocket_ = -1;
        }
        std::cout << "🛑 Sender Microservice stopped" << std::endl;
    }
};

// Global service for signal handling
SenderMicroservice* g_service = nullptr;

void signalHandler(int signal) {
    if (g_service) {
        std::cout << "\n🛑 Received signal " << signal << ", stopping service..." << std::endl;
        g_service->stop();
    }
    exit(0);
}

int main() {
    std::cout << "=== Sender Microservice ===" << std::endl;
    std::cout << "🌐 C++ TCP Market Data Streaming Service" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    SenderMicroservice service(8081);
    g_service = &service;
    
    if (!service.start()) {
        std::cerr << "❌ Failed to start sender microservice" << std::endl;
        return 1;
    }
    
    service.run();
    
    return 0;
}