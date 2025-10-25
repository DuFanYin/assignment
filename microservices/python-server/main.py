from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse, FileResponse
import subprocess
import json
import os
import time
import requests
from pathlib import Path
from typing import Dict, Any
import uvicorn

app = FastAPI(title="Order Book Streamer", description="Market Data Order Book Streaming System")

# Service URLs - configurable via environment variables for Docker
SENDER_SERVICE_URL = os.getenv("SENDER_SERVICE_URL", "http://127.0.0.1:8081")
RECEIVER_SERVICE_URL = os.getenv("RECEIVER_SERVICE_URL", "http://127.0.0.1:8082")

# Mount static files
app.mount("/static", StaticFiles(directory="static"), name="static")

@app.get("/", response_class=HTMLResponse)
async def get_index():
    """Serve the main HTML page"""
    return FileResponse("static/index.html")

@app.get("/api/status")
async def get_status():
    """Get current system status"""
    return {
        "status": "ready",
        "data_file_exists": os.path.exists("data/CLX5_mbo.dbn"),
        "output_file_exists": os.path.exists("data/order_book_output.json")
    }

@app.post("/api/start-streaming")
async def start_streaming():
    """Start both C++ microservices: sender and receiver"""
    try:
        # Check if data file exists
        data_file = Path("data/CLX5_mbo.dbn")
        if not data_file.exists():
            raise HTTPException(status_code=404, detail="Data file not found")
        
        # Remove old output file if it exists
        output_file = Path("data/order_book_output.json")
        if output_file.exists():
            output_file.unlink()
        
        print("🚀 Starting C++ microservices...")
        
        # Step 1: Call Sender Microservice to start streaming (persistent)
        print("📡 Starting Sender Microservice...")
        sender_url = f"{SENDER_SERVICE_URL}/start-streaming"
        print(f"📡 Calling sender at: {sender_url}")
        sender_response = requests.post(sender_url, timeout=60)
        
        if sender_response.status_code != 200:
            raise HTTPException(status_code=500, detail=f"Sender microservice failed: {sender_response.text}")
        
        sender_data = sender_response.json()
        print(f"✅ Sender started: {sender_data.get('message', 'Unknown')}")
        
        # Give sender time to start TCP server
        print("⏳ Waiting for sender to start TCP server...")
        time.sleep(2)
        
        # Step 2: Call Receiver Microservice to process data
        print("📥 Starting Receiver Microservice...")
        try:
            print("📥 Making HTTP request to receiver...")
            receiver_url = f"{RECEIVER_SERVICE_URL}/start-processing"
            print(f"📥 Making HTTP request to receiver at: {receiver_url}")
            receiver_response = requests.post(receiver_url, timeout=120)
            print(f"📥 Receiver response status: {receiver_response.status_code}")
            print(f"📥 Receiver response text: {receiver_response.text[:200]}...")
        except Exception as e:
            print(f"❌ Receiver request failed: {e}")
            raise HTTPException(status_code=500, detail=f"Receiver microservice failed: {e}")
        
        if receiver_response.status_code != 200:
            raise HTTPException(status_code=500, detail=f"Receiver microservice failed: {receiver_response.text}")
        
        receiver_data = receiver_response.json()
        print(f"✅ Receiver completed: {receiver_data.get('message', 'Unknown')}")
        
        # Give receiver time to finish writing the order book file
        print("⏳ Waiting for order book file to be written...")
        time.sleep(3)
        
        # Get processing statistics
        print("📊 Getting processing statistics...")
        try:
            stats_url = f"{RECEIVER_SERVICE_URL}/stats"
            stats_response = requests.get(stats_url, timeout=10)
            if stats_response.status_code == 200:
                receiver_data = stats_response.json()
                print(f"✅ Got processing statistics")
            else:
                print(f"⚠️ Could not get stats: {stats_response.status_code}")
        except Exception as e:
            print(f"⚠️ Stats request failed: {e}")
            # Keep the original receiver_data if stats fail
        
        # Step 3: Get Order Book Data
        print("📊 Retrieving order book data...")
        try:
            order_book_url = f"{RECEIVER_SERVICE_URL}/order-book"
            order_book_response = requests.get(order_book_url, timeout=10)
            print(f"📊 Order book response status: {order_book_response.status_code}")
            print(f"📊 Order book response length: {len(order_book_response.text)}")
        except Exception as e:
            print(f"❌ Order book request failed: {e}")
            raise HTTPException(status_code=500, detail=f"Failed to get order book: {e}")
        
        if order_book_response.status_code != 200:
            raise HTTPException(status_code=500, detail=f"Failed to get order book: {order_book_response.text}")
        
        # Parse order book data
        order_book_text = order_book_response.text.strip()
        order_book_data = []
        
        if order_book_text:
            for line in order_book_text.split('\n'):
                line = line.strip()
                if line:
                    try:
                        order_book_data.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        
        # Get final order book state (last entry)
        final_state = order_book_data[-1] if order_book_data else {}
        
        result = {
            "status": "success",
            "message": "Microservices completed successfully",
            "total_records": len(order_book_data),
            "final_order_book": final_state,
            "sender_stats": sender_data,
            "receiver_stats": receiver_data,
            "data": order_book_data  # Add the actual order book data
        }
        
        print(f"✅ Returning result with {len(order_book_data)} records")
        return result
        
    except requests.exceptions.RequestException as e:
        raise HTTPException(status_code=500, detail=f"Microservice communication failed: {str(e)}")
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error: {str(e)}")

@app.get("/api/order-book")
async def get_order_book():
    """Get the current order book data"""
    output_file = Path("data/order_book_output.json")
    if not output_file.exists():
        raise HTTPException(status_code=404, detail="No order book data available")
    
    try:
        with open(output_file, "r") as f:
            lines = f.readlines()
            order_book_data = []
            for line in lines:
                line = line.strip()
                if line:
                    try:
                        order_book_data.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        
        return {
            "status": "success",
            "total_records": len(order_book_data),
            "data": order_book_data
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error reading order book: {str(e)}")

@app.get("/api/microservices/status")
async def get_microservices_status():
    """Get status of C++ microservices"""
    status = {
        "python_server": {
            "status": "ready",
            "port": 8000,
            "description": "FastAPI web server"
        },
        "sender_microservice": {
            "status": "unknown",
            "port": 8081,
            "description": "C++ TCP sender microservice"
        },
        "receiver_microservice": {
            "status": "unknown", 
            "port": 8082,
            "description": "C++ TCP receiver with order book"
        },
        "data_file": {
            "exists": os.path.exists("data/CLX5_mbo.dbn"),
            "path": str(Path("data/CLX5_mbo.dbn").resolve())
        },
        "output_file": {
            "exists": os.path.exists("data/order_book_output.json"),
            "path": str(Path("data/order_book_output.json").resolve())
        }
    }
    
    # Check microservice status
    try:
        sender_status_url = f"{SENDER_SERVICE_URL}/status"
        sender_response = requests.get(sender_status_url, timeout=2)
        if sender_response.status_code == 200:
            status["sender_microservice"]["status"] = "ready"
    except:
        status["sender_microservice"]["status"] = "not_running"
    
    try:
        receiver_status_url = f"{RECEIVER_SERVICE_URL}/status"
        receiver_response = requests.get(receiver_status_url, timeout=2)
        if receiver_response.status_code == 200:
            status["receiver_microservice"]["status"] = "ready"
    except:
        status["receiver_microservice"]["status"] = "not_running"
    
    return status

if __name__ == "__main__":
    print("🌐 Starting Order Book Streamer Server...")
    print("📊 FastAPI backend with C++ order book integration")
    print("🔗 Access the web interface at: http://localhost:8000")
    
    uvicorn.run(
        "main:app",
        host="0.0.0.0",
        port=8000,
        reload=True,
        log_level="info"
    )
