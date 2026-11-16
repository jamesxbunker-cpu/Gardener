# Gardener Dashboard

A Python FastAPI dashboard for Gardener. It connects to the `--dashboard` TCP stream, parses the registry and events, and provides a web UI.

## Setup

1. Install Python dependencies:

        pip install -r requirements.txt

2. Start Gardener with the dashboard flag enabled:

        ./build/gardener --channels configs/channels.toml --detect --dashboard 5555 < your_data.jsonl

3. Start the dashboard:

        python dashboard.py

4. Open http://localhost:8000 in your browser.