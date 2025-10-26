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
        
        # Step 1: Start sender
        print("📡 Starting sender...")
        sender_response = requests.post("http://127.0.0.1:8081/start-streaming", timeout=60)
        
        if sender_response.status_code != 200:
            raise HTTPException(status_code=500, detail=f"Sender microservice failed: {sender_response.text}")
        
        sender_data = sender_response.json()
        print(f"✅ Sender: {sender_data.get('message', 'OK')}")
        time.sleep(2)
        
        # Step 2: Start receiver
        print("📥 Starting receiver...")
        receiver_response = requests.post("http://127.0.0.1:8082/start-processing", timeout=120)
        
        if receiver_response.status_code != 200:
            raise HTTPException(status_code=500, detail=f"Receiver microservice failed: {receiver_response.text}")
        
        receiver_data = receiver_response.json()
        print(f"✅ Receiver: {receiver_data.get('message', 'OK')}")
        time.sleep(3)
        
        # Step 3: Get order book
        print("📊 Getting order book...")
        order_book_response = requests.get("http://127.0.0.1:8082/order-book", timeout=10)
        
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
            "data": order_book_data
        }
        
        print(f"✅ Complete: {len(order_book_data)} records")
        return result
        
    except requests.exceptions.RequestException as e:
        print(f"❌ Connection failed: {str(e)}")
        print("\nTo debug, check logs:")
        print("  tail -n 50 sender.log")
        print("  tail -n 50 receiver.log")
        raise HTTPException(status_code=500, detail=f"Microservice communication failed: {str(e)}")
    except Exception as e:
        print(f"❌ Error: {str(e)}")
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
        sender_response = requests.get("http://127.0.0.1:8081/status", timeout=2)
        if sender_response.status_code == 200:
            status["sender_microservice"]["status"] = "ready"
    except:
        status["sender_microservice"]["status"] = "not_running"
    
    try:
        receiver_response = requests.get("http://127.0.0.1:8082/status", timeout=2)
        if receiver_response.status_code == 200:
            status["receiver_microservice"]["status"] = "ready"
    except:
        status["receiver_microservice"]["status"] = "not_running"
    
    return status

if __name__ == "__main__":
    uvicorn.run(
        "main:app",
        host="0.0.0.0",
        port=8000,
        reload=True,
        log_level="info"
    )
