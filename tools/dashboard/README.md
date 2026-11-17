# Gardener Dashboard

A lightweight, real-time web dashboard for Gardener. 

It connects to Gardener's `--dashboard <port>` TCP JSONL stream, parses the live telemetry and anomaly events, and updates a web interface using FastAPI, Jinja2, and htmx.

## Prerequisites

* Python 3.8+
* Gardener running with `--dashboard <port>` (e.g., 5555).

## Installation

1. Navigate to this directory:

    cd tools/dashboard

2. Install the Python dependencies:

    py -m pip install -r requirements.txt

## Running

You need two terminal windows.

Terminal 1: Start the Dashboard

    py dashboard.py

The web server will start on http://localhost:8000 (or 8001 if you changed the port). Keep this terminal open.

Terminal 2: Start Gardener
Go back to the root of the Gardener project, and run the binary with the dashboard flag enabled. 

If you have a saved JSONL file:

    Get-Content your_data.jsonl | .\build\gardener --channels configs/channels.toml --detect --dashboard 5555

If you are streaming live data from the simulator (gensim):

    py tools\gensim\gensim.py --scenario tools\gensim\scenarios\sensor_drift.yaml --duration 60 --speed 5 |
        .\build\gardener --channels configs/channels.toml --baseline build/normal.bl --detect --dashboard 5555

## Usage

Once both are running, open your web browser and navigate to:
http://localhost:8000

* Status Indicator: The top right corner will show a green "Connected" badge with the number of registered channels. If Gardener stops, it will turn red ("Disconnected").
* Live Events: The main list will populate in real-time as Gardener detects anomalies. 
* Event Details: Each event card shows the timestamp, the channel name (resolved from the registry), the observed value, and the threshold that was crossed. Critical events are highlighted in red, warnings in orange.