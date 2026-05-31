#!/usr/bin/env bash
# Smoke test the RPC.
PORT="${FSUAE_RPC_PORT:-8765}"
curl -s "http://127.0.0.1:$PORT/v1/ping"
echo
