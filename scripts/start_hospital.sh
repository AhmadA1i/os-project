#!/bin/bash
# start_hospital.sh - Initialize IPC resources and launch admissions

TRIAGE_FIFO="/tmp/triage_fifo"
DISCHARGE_FIFO="/tmp/discharge_fifo"
PID_FILE="/tmp/admissions.pid"

echo "============================================"
echo "  INITIALIZING HOSPITAL ENVIRONMENT..."
echo "============================================"

# Step 1: Clean up old resources
echo "Cleaning up stale resources..."
rm -f "$TRIAGE_FIFO"
rm -f "$DISCHARGE_FIFO"
rm -f "$PID_FILE"
# Clean old shared memory and semaphores
ipcrm -M 0xBEDF00D 2>/dev/null

# Step 2: Compile the project
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.." || { echo "Error: Cannot move to project root"; exit 1; }

make clean 2>/dev/null
make all

if [ $? -ne 0 ]; then
    echo "Compilation failed! Hospital does not start"
    exit 1
fi

echo "Compilation successful."

# Step 3: Create FIFOs
mkfifo "$TRIAGE_FIFO"
mkfifo "$DISCHARGE_FIFO"
chmod 666 "$TRIAGE_FIFO"
chmod 666 "$DISCHARGE_FIFO"
echo "FIFOs created."

# Step 4: Launch admissions in background
echo "Launching Admissions Manager..."
./admissions &

adm=$!
echo "$adm" > "$PID_FILE"
echo "Admissions Manager started with PID: $adm"

# Step 5: Success banner
echo ""
echo "============================================"
echo "  HOSPITAL IS NOW OPEN AND RUNNING"
echo "============================================"
echo "Admit patients using:"
echo "  ./scripts/Triage.sh \"Patient Name\" <age> <severity>"
echo ""
echo "To shut down:"
echo "  ./scripts/stop_hospital.sh"
echo "============================================"

exit 0
