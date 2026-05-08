# Hospital Admissions Management System

This project simulates a hospital admissions workflow using operating systems concepts such as shared memory, semaphores, FIFO communication, threads, mutexes, condition variables, and process creation. Patients are triaged, queued by priority, assigned beds using a best-fit strategy, simulated during treatment, and discharged back into the system.

## Project Description

The system is split into a few cooperating parts:

- `scripts/Triage.sh` collects patient details, computes priority from severity, and sends a formatted record to the triage FIFO.
- `admissions` is the central hospital manager. It maintains ward state, processes the queue, allocates beds, launches patient simulators, and handles discharge coordination.
- `patient_simulator` represents an admitted patient. It sleeps for a treatment duration based on bed type, then writes its patient ID to the discharge FIFO.
- `scripts/start_hospital.sh` prepares the IPC resources and starts the admissions manager.
- `scripts/stop_hospital.sh` shuts the system down cleanly and removes temporary resources.

## Build Instructions

This project uses a POSIX-style toolchain.

Requirements:

- `gcc`
- `make`
- Bash shell
- POSIX IPC support (`mkfifo`, shared memory, semaphores)

Build everything from the project root:

```bash
make
```

If you want to rebuild from scratch:

```bash
make clean
make
```

## How to Run

Recommended startup flow:

```bash
./scripts/start_hospital.sh
```

That script:

1. Removes stale FIFOs, PID files, shared memory, and old resources.
2. Compiles the project.
3. Creates `/tmp/triage_fifo` and `/tmp/discharge_fifo`.
4. Starts the `admissions` process in the background.

Send a patient into the system with:

```bash
./scripts/Triage.sh "Ali Khan" 25 9
```

Use the shutdown script when you are done:

```bash
./scripts/stop_hospital.sh
```

### Manual run option

If you prefer to control each step yourself:

```bash
mkfifo /tmp/triage_fifo /tmp/discharge_fifo
./admissions
```

Then open another terminal and use `./scripts/Triage.sh` to submit patients.

## OS Concepts Demonstrated

- **Shared Memory:** `WardStatus` is stored in shared memory so the admissions process can track beds and patients efficiently.
- **Semaphores:** Named semaphores limit ICU and Isolation admissions, while unnamed semaphores provide bounded producer-consumer synchronization for the queue.
- **FIFOs / Named Pipes:** Triage and discharge events move through FIFOs between independent processes.
- **Threads:** The admissions manager uses multiple threads for reception, scheduling, and nursing/discharge handling.
- **Mutexes:** Mutexes protect shared ward state, the patient queue, and discharge routing data.
- **Condition Variables:** Condition variables wake the scheduler when beds free up and coordinate queue and discharge events.
- **Process Management:** The admissions manager uses `fork()` and `execv()` to launch the patient simulator.
- **Signals:** `SIGTERM` and `SIGCHLD` are used for graceful shutdown and child reaping.
- **Memory Management / Best-Fit:** Bed partitions are allocated using best-fit selection, split when needed, and coalesced when beds are freed.

## My Contribution

My contribution to this project was to build the core operating-systems workflow and the viva preparation material. In particular, I implemented and documented:

- The admissions manager workflow in `src/admissions.c`
- Shared-memory ward state and bed partition logic in `src/hospital.h`
- Priority-based queueing, aging, and phase-based synchronization
- The threaded Phase 3 design with receptionist, scheduler, and nurse roles
- The `vivaprep.md` notes that explain each phase and the viva questions

## Project Notes

- Patient input format:

```text
ID|NAME|AGE|SEVERITY|PRIORITY|CARE_UNITS|BED_TYPE|TIMESTAMP
```

- Bed types used by the system:
  - `ICU`
  - `ISOLATION`
  - `GENERAL`

- The main hospital configuration is defined in `src/hospital.h`.

## Cleaning Up

To remove build outputs and temporary IPC files:

```bash
make clean
./scripts/stop_hospital.sh
```

## Test Findings (Log Verification)

The following findings were verified from the provided execution logs.

1. Receptionist receives patients from triage FIFO: PASS
  Evidence: repeated log lines such as `[Receptionist] Patient #1 received: Ali Khan ...` and `[Receptionist] Priority: ...`.

2. Scheduler dequeues and allocates beds (best-fit behavior visible in partition splits): PASS
  Evidence: repeated lines like `[Scheduler] Bed allocated for ...: partition ...` and ward snapshots showing partition split/merge behavior.

3. Nurse threads process discharge and free/coalesce beds: PASS
  Evidence: lines such as `[Nurse-ICU] Discharge signal received ...`, `[Nurse-GENERAL] ...`, `[Nurse-ISOLATION] ...`, followed by `[Admissions] Bed freed ...`.

4. ICU and Isolation semaphore limits block admission and release after discharge: PASS
  Evidence: in ICU stress test (`ICU_1` to `ICU_8`), first 4 were admitted, remaining patients were received but only admitted after ICU discharges (e.g., ICU_5 admitted after ICU_1 discharge). Similar behavior observed in Isolation stress test.

5. Scheduler waits on bed-freed condition when no suitable partition exists: PARTIAL / NOT DIRECTLY SHOWN
  Evidence needed: explicit scheduler log `[Scheduler] No suitable bed ... waiting for a bed to be freed...`.
  Observation: this exact waiting log did not appear in the shared output, so the condition-variable wait path is not directly demonstrated by the current run.

6. Build status:
  - Build succeeded.
  - Warning present: `effective_priority` defined but not used.

Suggested extra test to explicitly show scheduler waiting path:

```bash
# Keep system running, then quickly saturate one bed class and continue sending same class
for i in {1..20}; do ./scripts/Triage.sh "WAIT_$i" 55 10; done
```

Expected additional evidence:
- `[Scheduler] No suitable bed for ...; waiting for a bed to be freed...`
- Later admission of the same patient after a nurse frees a bed.
