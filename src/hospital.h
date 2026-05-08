#ifndef HOSPITAL_H
#define HOSPITAL_H

#include <time.h>

/* Ward Configuration */
#define MAX_BEDS 20
#define ICU_BEDS 4
#define ISOLATION_BEDS 4
#define GENERAL_BEDS 12

#define ICU_CARE_UNITS 3
#define ISOLATION_CARE_UNITS 2
#define GENERAL_CARE_UNITS 1

#define TOTAL_WARD_UNITS (ICU_BEDS * ICU_CARE_UNITS + \
                          ISOLATION_BEDS * ISOLATION_CARE_UNITS + \
                          GENERAL_BEDS * GENERAL_CARE_UNITS)

#define ICU_CAPACITY 4
#define ISOLATION_CAPACITY 4

/* Treatment durations (seconds) */
#define ICU_MIN_TREATMENT 5
#define ICU_MAX_TREATMENT 15
#define GENERAL_MIN_TREATMENT 2
#define GENERAL_MAX_TREATMENT 8
#define ISOLATION_MIN_TREATMENT 3
#define ISOLATION_MAX_TREATMENT 10

/* IPC paths and keys */
#define SHM_KEY 0xBEDF00D
#define TRIAGE_FIFO "/tmp/triage_fifo"
#define DISCHARGE_FIFO "/tmp/discharge_fifo"

/* Semaphore names */
#define SEM_ICU_LIMIT "/sem_icu_limit"
#define SEM_ISO_LIMIT "/sem_iso_limit"

/* Queue limits */
#define MAX_QUEUE_SIZE 20
#define MAX_NAME_LEN 64

/* Aging threshold seconds (reduce priority value every AGE_THRESHOLD seconds) */
#define AGE_THRESHOLD_SECONDS 30

/* Patient Record */
typedef struct {
    int    patient_id;
    char   name[MAX_NAME_LEN];
    int    age;
    int    severity;
    int    priority;
    int    care_units;
    char   bed_type[16];
    time_t arrival_time;
} PatientRecord;

/* Bed Partition (memory management) */
typedef struct {
    int  partition_id;
    int  start_unit;
    int  size;
    int  is_free;
    int  patient_id;
    char bed_type[16];
} BedPartition;

/* Ward Status (lives in shared memory) */
typedef struct {
    BedPartition partitions[MAX_BEDS];
    int          num_partitions;
    int          total_patients_served;
    int          active_patients;
    /* Waiting queue stored in shared memory for Phase 2 */
    PatientRecord queue[MAX_QUEUE_SIZE];
    int           queue_count;
} WardStatus;

#endif
