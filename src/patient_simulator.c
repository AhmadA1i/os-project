/*
 * patient_simulator.c - Simulates a patient's hospital stay
 *
 * Called by admissions via fork() + execv().
 * Receives patient data as command-line arguments.
 * Sleeps for a random treatment duration, then writes
 * patient ID to the discharge FIFO to signal completion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "hospital.h"

int main(int argc, char *argv[]) {
    /*
     * argv[0] = "./patient_simulator"
     * argv[1] = patient_id
     * argv[2] = patient_name
     * argv[3] = triage_priority
     * argv[4] = bed_partition_id
     * argv[5] = bed_type ("ICU", "GENERAL", "ISOLATION")
     */

    if (argc < 6) {
        printf("[Patient] Error: not enough arguments.\n");
        printf("[Patient] Usage: ./patient_simulator <id> <name> <priority> <bed_id> <bed_type>\n");
        return 1;
    }

    int patient_id = atoi(argv[1]);
    char *name = argv[2];
    int priority = atoi(argv[3]);
    int bed_id = atoi(argv[4]);
    char *bed_type = argv[5];

    /* Seed random with PID for unique durations */
    srand(time(NULL) ^ getpid());

    /* Calculate treatment duration based on bed type */
    int min_t, max_t;
    if (strcmp(bed_type, "ICU") == 0) {
        min_t = ICU_MIN_TREATMENT;
        max_t = ICU_MAX_TREATMENT;
    } else if (strcmp(bed_type, "ISOLATION") == 0) {
        min_t = ISOLATION_MIN_TREATMENT;
        max_t = ISOLATION_MAX_TREATMENT;
    } else {
        min_t = GENERAL_MIN_TREATMENT;
        max_t = GENERAL_MAX_TREATMENT;
    }

    int duration = min_t + rand() % (max_t - min_t + 1);

    printf("[Patient %d] %s admitted to %s (bed %d, priority %d)\n",
           patient_id, name, bed_type, bed_id, priority);
    printf("[Patient %d] Treatment duration: %d seconds\n",
           patient_id, duration);
    fflush(stdout);

    /* Simulate treatment */
    sleep(duration);

    printf("[Patient %d] %s treatment complete. Discharging.\n",
           patient_id, name);
    fflush(stdout);

    /* Write patient ID to discharge FIFO */
    int fd = open(DISCHARGE_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%d\n", patient_id);
        write(fd, buf, len);
        close(fd);
    } else {
        printf("[Patient %d] Warning: could not open discharge FIFO.\n", patient_id);
    }

    return 0;
}
