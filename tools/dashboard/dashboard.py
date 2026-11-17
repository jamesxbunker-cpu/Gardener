#!/usr/bin/env python3
import asyncio
import json
import socket
import threading
import time
import os
from collections import deque
from typing import List, Dict, Any
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates

GARDENER_HOST = '127.0.0.1'
GARDENER_PORT = 5556
events = deque(maxlen=50)
registry = {}
connected = False

app = FastAPI()
base_dir = os.path.dirname(os.path.abspath(__file__))
templates = Jinja2Templates(directory=os.path.join(base_dir, "templates"))

def listen_to_gardener():
    global connected, registry
    while True:
        try:
            print(f"Connecting to Gardener at {GARDENER_HOST}:{GARDENER_PORT}...")
            with socket.create_connection((GARDENER_HOST, GARDENER_PORT)) as s:
                connected = True
                print("Connected!")
                buffer = ""
                while True:
                    # This blocks indefinitely waiting for data. No timeout.
                    data = s.recv(4096)
                    if not data:
                        break
                    buffer += data.decode('utf-8')
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        if not line.strip():
                            continue
                        try:
                            msg = json.loads(line)
                            if msg.get('type') == 'registry':
                                for ch in msg['channels']:
                                    registry[ch['id']] = ch
                            elif 'detector' in msg:
                                msg['_recv_time'] = time.time()
                                events.appendleft(msg)
                        except json.JSONDecodeError:
                            pass
        except ConnectionRefusedError:
            pass
        except Exception as e:
            print(f"Error: {e}")
        finally:
            connected = False
        time.sleep(2)

threading.Thread(target=listen_to_gardener, daemon=True).start()

@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    return templates.TemplateResponse(
        request=request, 
        name="index.html", 
        context={"connected": connected}
    )

@app.get("/api/events")
async def get_events():
    enriched_events = []
    for e in events:
        e_copy = e.copy()
        ch_id = e.get('ch')
        if ch_id in registry:
            e_copy['ch_name'] = registry[ch_id]['name']
        else:
            e_copy['ch_name'] = f"ID {ch_id}"
        enriched_events.append(e_copy)
    
    return {
        "connected": connected,
        "events": enriched_events,
        "registry_count": len(registry)
    }

if __name__ == "__main__":
    print("Starting Dashboard on http://localhost:8001")
    uvicorn.run(app, host="0.0.0.0", port=8001)