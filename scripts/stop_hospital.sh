#!/bin/bash
# stop_hospital.sh - Gracefully shutdown the hospital system

PID_FILE="/tmp/admissions.pid"
TRIAGE_FIFO="/tmp/triage_fifo"
DISCHARGE_FIFO="/tmp/discharge_fifo"

echo "============================================"
echo "  SHUTTING DOWN HOSPITAL..."
echo "============================================"

# Step 1: Kill admissions process
if [[ -f "$PID_FILE" ]]; then
    ADMISSIONS_PID=$(cat "$PID_FILE")
    echo "Found Admissions Manager (PID: $ADMISSIONS_PID)"

    # Unblock any stuck FIFO reads
    echo "SHUTDOWN" > "$TRIAGE_FIFO" 2>/dev/null

    echo "Sending SIGTERM..."
    kill -15 "$ADMISSIONS_PID" 2>/dev/null

    if [[ $? -eq 0 ]]; then
        # Wait up to 5 seconds for graceful shutdown
        sleep 2
        # Force kill if still running
        kill -0 "$ADMISSIONS_PID" 2>/dev/null && kill -9 "$ADMISSIONS_PID" 2>/dev/null
        echo "Admissions Manager stopped."
    else
        echo "Process not found. May have already stopped."
    fi

    # Kill any remaining patient_simulator processes
    pkill -f patient_simulator 2>/dev/null

    rm -f "$PID_FILE"
else
    echo "No PID file found. Hospital may not be running."
fi

# Step 2: Remove FIFOs
if [[ -p "$TRIAGE_FIFO" ]]; then
    rm -f "$TRIAGE_FIFO"
    echo "Removed triage FIFO."
fi

if [[ -p "$DISCHARGE_FIFO" ]]; then
    rm -f "$DISCHARGE_FIFO"
    echo "Removed discharge FIFO."
fi

# Step 3: Clean shared memory and semaphores
ipcrm -M 0xBEDF00D 2>/dev/null

echo ""
echo "Hospital shutdown complete."
echo "============================================"

exit 0
