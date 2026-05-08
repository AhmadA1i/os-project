CC = gcc
CFLAGS = -Wall -Wextra -pthread
SRC_DIR = src

all: admissions patient_simulator
	@echo "Build complete."

admissions: $(SRC_DIR)/admissions.c $(SRC_DIR)/hospital.h
	$(CC) $(CFLAGS) -o admissions $(SRC_DIR)/admissions.c -lrt

patient_simulator: $(SRC_DIR)/patient_simulator.c $(SRC_DIR)/hospital.h
	$(CC) $(CFLAGS) -o patient_simulator $(SRC_DIR)/patient_simulator.c

run: all
	@cd scripts && ./start_hospital.sh

stop:
	@cd scripts && ./stop_hospital.sh

clean:
	rm -f admissions patient_simulator
	rm -f /tmp/triage_fifo /tmp/discharge_fifo /tmp/admissions.pid

test: all
	@echo "--- Test 1: Critical Patient ---"
	@cd scripts && ./Triage.sh "Ali Khan" 25 9
	@echo ""
	@echo "--- Test 2: Low Priority ---"
	@cd scripts && ./Triage.sh "Sara Ahmed" 30 3

.PHONY: all clean run stop test
