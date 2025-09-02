#!/usr/bin/env bash
set -euo pipefail
echo "== Building backend =="
cd backend
make
echo "== Starting server on :5001 =="
./server
