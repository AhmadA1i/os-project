/*
 * admissions.c - Central Hospital Admissions Manager
 *
 * This process:
 *   1. Creates shared memory for ward status
 *   2. Initializes bed partitions (ICU, Isolation, General)
 *   3. Creates semaphores for ICU/Isolation capacity
 *   4. Reads patient records from triage FIFO
 *   5. Allocates beds using Best-Fit algorithm
 *   6. Forks child processes (patient_simulator) for each patient
 *   7. Handles discharge via a separate thread
 *   8. Coalesces freed partitions on discharge
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <semaphore.h>
#include "hospital.h"

/* ---- Global State ---- */
volatile sig_atomic_t running = 1;
int shm_id = -1;
WardStatus *ward = NULL;
sem_t *sem_icu = NULL;
sem_t *sem_iso = NULL;
sem_t queue_slots_sem;
sem_t queue_items_sem;
pthread_mutex_t bed_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t discharge_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t bed_freed_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t discharge_conds[3] = {
    PTHREAD_COND_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    PTHREAD_COND_INITIALIZER
};
int discharge_queue[3][MAX_QUEUE_SIZE];
int discharge_count[3] = {0, 0, 0};
int patient_counter = 0;

enum BedTypeIndex {
    BED_ICU = 0,
    BED_ISOLATION = 1,
    BED_GENERAL = 2
};

static const char *BED_LABELS[3] = {"ICU", "ISOLATION", "GENERAL"};

/* ---- Forward Declarations ---- */
void spawn_patient(PatientRecord *p, int bed_idx);
void print_ward(void);
int allocate_bed(int care_units, const char *bed_type, int patient_id);
void free_bed(int patient_id);
PatientRecord parse_record(const char *line);

static const char *normalize_bed_type(const char *bed_type);
static int bed_type_index(const char *bed_type);
static void queue_push_locked(const PatientRecord *p);
static PatientRecord queue_pop_locked(void);
static int queue_count_locked(void);
static int effective_priority(const PatientRecord *p);
static int lookup_patient_bed_type_index(int patient_id);
static void enqueue_discharge(int bed_index, int patient_id);
static int dequeue_discharge(int bed_index, int *patient_id);
void *receptionist_thread(void *arg);
void *scheduler_thread(void *arg);
void *nurse_thread(void *arg);
void *discharge_listener_thread(void *arg);

/* ---- Signal Handlers ---- */
void handle_sigterm(int sig) {
    (void)sig;
    running = 0;
}

void handle_sigchld(int sig) {
    (void)sig;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

/* ---- Initialize Ward Partitions ---- */
void init_ward(void) {
    int idx = 0, unit = 0;

    /* ICU zone */
    ward->partitions[idx].partition_id = idx;
    ward->partitions[idx].start_unit = unit;
    ward->partitions[idx].size = ICU_BEDS * ICU_CARE_UNITS;
    ward->partitions[idx].is_free = 1;
    ward->partitions[idx].patient_id = -1;
    strncpy(ward->partitions[idx].bed_type, "ICU", 15);
    unit += ward->partitions[idx].size;
    idx++;

    /* Isolation zone */
    ward->partitions[idx].partition_id = idx;
    ward->partitions[idx].start_unit = unit;
    ward->partitions[idx].size = ISOLATION_BEDS * ISOLATION_CARE_UNITS;
    ward->partitions[idx].is_free = 1;
    ward->partitions[idx].patient_id = -1;
    strncpy(ward->partitions[idx].bed_type, "ISOLATION", 15);
    unit += ward->partitions[idx].size;
    idx++;

    /* General zone */
    ward->partitions[idx].partition_id = idx;
    ward->partitions[idx].start_unit = unit;
    ward->partitions[idx].size = GENERAL_BEDS * GENERAL_CARE_UNITS;
    ward->partitions[idx].is_free = 1;
    ward->partitions[idx].patient_id = -1;
    strncpy(ward->partitions[idx].bed_type, "GENERAL", 15);
    idx++;

    ward->num_partitions = idx;
    ward->total_patients_served = 0;
    ward->active_patients = 0;
    ward->queue_count = 0;
}

/* ---- Best-Fit Bed Allocation ---- */
static int allocate_bed_locked(int care_units, const char *bed_type, int patient_id) {
    int best = -1, best_size = 99999;

    for (int i = 0; i < ward->num_partitions; i++) {
        if (ward->partitions[i].is_free &&
            strcmp(ward->partitions[i].bed_type, bed_type) == 0 &&
            ward->partitions[i].size >= care_units &&
            ward->partitions[i].size < best_size) {
            best = i;
            best_size = ward->partitions[i].size;
        }
    }

    if (best == -1)
        return -1;

    if (ward->partitions[best].size > care_units &&
        ward->num_partitions < MAX_BEDS) {
        for (int i = ward->num_partitions; i > best + 1; i--)
            ward->partitions[i] = ward->partitions[i - 1];

        int nxt = best + 1;
        ward->partitions[nxt].partition_id = ward->num_partitions;
        ward->partitions[nxt].start_unit = ward->partitions[best].start_unit + care_units;
        ward->partitions[nxt].size = ward->partitions[best].size - care_units;
        ward->partitions[nxt].is_free = 1;
        ward->partitions[nxt].patient_id = -1;
        strncpy(ward->partitions[nxt].bed_type, bed_type, 15);
        ward->num_partitions++;
        ward->partitions[best].size = care_units;
    }

    ward->partitions[best].is_free = 0;
    ward->partitions[best].patient_id = patient_id;
    ward->active_patients++;
    return best;
}

int allocate_bed(int care_units, const char *bed_type, int patient_id) {
    int bed_idx;
    pthread_mutex_lock(&bed_mutex);
    bed_idx = allocate_bed_locked(care_units, bed_type, patient_id);
    pthread_mutex_unlock(&bed_mutex);
    return bed_idx;
}

static void queue_push_locked(const PatientRecord *p) {
    int i = ward->queue_count;
    while (i > 0) {
        PatientRecord *prev = &ward->queue[i - 1];
        if (prev->priority < p->priority) break;
        if (prev->priority == p->priority && prev->arrival_time <= p->arrival_time) break;
        ward->queue[i] = ward->queue[i - 1];
        i--;
    }
    ward->queue[i] = *p;
    ward->queue_count++;
}

static PatientRecord queue_pop_locked(void) {
    PatientRecord p = ward->queue[0];
    for (int i = 0; i < ward->queue_count - 1; i++)
        ward->queue[i] = ward->queue[i + 1];
    ward->queue_count--;
    return p;
}

static int queue_count_locked(void) {
    return ward->queue_count;
}

static int effective_priority(const PatientRecord *p) {
    time_t now = time(NULL);
    int waited = (int)(now - p->arrival_time);
    int reduce = waited / AGE_THRESHOLD_SECONDS;
    int eff = p->priority - reduce;
    if (eff < 1) eff = 1;
    return eff;
}

static const char *normalize_bed_type(const char *bed_type) {
    if (strcmp(bed_type, "Isolation") == 0)
        return "ISOLATION";
    return bed_type;
}

static int bed_type_index(const char *bed_type) {
    if (strcmp(bed_type, "ICU") == 0) return BED_ICU;
    if (strcmp(bed_type, "ISOLATION") == 0) return BED_ISOLATION;
    return BED_GENERAL;
}

static int lookup_patient_bed_type_index(int patient_id) {
    int result = BED_GENERAL;

    pthread_mutex_lock(&bed_mutex);
    for (int i = 0; i < ward->num_partitions; i++) {
        if (!ward->partitions[i].is_free && ward->partitions[i].patient_id == patient_id) {
            result = bed_type_index(ward->partitions[i].bed_type);
            break;
        }
    }
    pthread_mutex_unlock(&bed_mutex);
    return result;
}

static void enqueue_discharge(int bed_index, int patient_id) {
    pthread_mutex_lock(&discharge_mutex);
    if (discharge_count[bed_index] < MAX_QUEUE_SIZE) {
        discharge_queue[bed_index][discharge_count[bed_index]++] = patient_id;
        pthread_cond_signal(&discharge_conds[bed_index]);
    }
    pthread_mutex_unlock(&discharge_mutex);
}

static int dequeue_discharge(int bed_index, int *patient_id) {
    int ok = 0;

    pthread_mutex_lock(&discharge_mutex);
    if (discharge_count[bed_index] > 0) {
        *patient_id = discharge_queue[bed_index][0];
        for (int i = 0; i < discharge_count[bed_index] - 1; i++)
            discharge_queue[bed_index][i] = discharge_queue[bed_index][i + 1];
        discharge_count[bed_index]--;
        ok = 1;
    }
    pthread_mutex_unlock(&discharge_mutex);
    return ok;
}

static int pop_queue_for_scheduler(PatientRecord *p) {
    int has_item = 0;

    pthread_mutex_lock(&queue_mutex);
    if (queue_count_locked() > 0) {
        *p = queue_pop_locked();
        has_item = 1;
        pthread_cond_broadcast(&queue_not_full);
    }
    pthread_mutex_unlock(&queue_mutex);
    return has_item;
}

void *receptionist_thread(void *arg) {
    (void)arg;

    while (running) {
        FILE *fp = fopen(TRIAGE_FIFO, "r");
        if (!fp) {
            if (!running) break;
            sleep(1);
            continue;
        }

        char line[512];
        while (running && fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "SHUTDOWN", 8) == 0) {
                running = 0;
                sem_post(&queue_items_sem);
                sem_post(&queue_slots_sem);
                sem_post(sem_icu);
                sem_post(sem_iso);
                pthread_mutex_lock(&queue_mutex);
                pthread_cond_broadcast(&queue_not_empty);
                pthread_cond_broadcast(&queue_not_full);
                pthread_mutex_unlock(&queue_mutex);
                pthread_mutex_lock(&bed_mutex);
                pthread_cond_broadcast(&bed_freed_cond);
                pthread_mutex_unlock(&bed_mutex);
                pthread_mutex_lock(&discharge_mutex);
                for (int i = 0; i < 3; i++)
                    pthread_cond_broadcast(&discharge_conds[i]);
                pthread_mutex_unlock(&discharge_mutex);
                break;
            }

            PatientRecord p = parse_record(line);
            if (p.patient_id <= 0) continue;
            if (strcmp(p.bed_type, "Isolation") == 0)
                strncpy(p.bed_type, "ISOLATION", sizeof(p.bed_type) - 1);

            sem_wait(&queue_slots_sem);
            pthread_mutex_lock(&queue_mutex);
            queue_push_locked(&p);
            pthread_cond_signal(&queue_not_empty);
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&queue_items_sem);

            patient_counter++;
            printf("\n[Receptionist] Patient #%d received: %s (age %d, severity %d)\n",
                   patient_counter, p.name, p.age, p.severity);
            printf("[Receptionist] Priority: %d | Bed Type: %s | Care Units: %d\n",
                   p.priority, p.bed_type, p.care_units);
        }

        fclose(fp);
        if (running)
            sleep(1);
    }

    pthread_mutex_lock(&queue_mutex);
    pthread_cond_broadcast(&queue_not_empty);
    pthread_cond_broadcast(&queue_not_full);
    pthread_mutex_unlock(&queue_mutex);
    return NULL;
}

void *scheduler_thread(void *arg) {
    (void)arg;

    while (running) {
        while (running && sem_wait(&queue_items_sem) != 0 && errno == EINTR) {
        }
        if (!running) break;

        PatientRecord p;
        if (!pop_queue_for_scheduler(&p))
            continue;
        sem_post(&queue_slots_sem);

        const char *match_type = normalize_bed_type(p.bed_type);
        if (strcmp(match_type, "ICU") == 0) {
            while (running && sem_wait(sem_icu) != 0 && errno == EINTR) {
            }
        } else if (strcmp(match_type, "ISOLATION") == 0) {
            while (running && sem_wait(sem_iso) != 0 && errno == EINTR) {
            }
        }

        int bed_idx = -1;
        pthread_mutex_lock(&bed_mutex);
        while (running) {
            bed_idx = allocate_bed_locked(p.care_units, match_type, p.patient_id);
            if (bed_idx >= 0)
                break;
            printf("[Scheduler] No suitable bed for %s; waiting for a bed to be freed...\n", p.name);
            pthread_cond_wait(&bed_freed_cond, &bed_mutex);
        }
        pthread_mutex_unlock(&bed_mutex);

        if (!running)
            break;

        printf("[Scheduler] Bed allocated for %s: partition %d\n", p.name, bed_idx);
        print_ward();
        spawn_patient(&p, bed_idx);
    }

    return NULL;
}

void *nurse_thread(void *arg) {
    int bed_index = *(int *)arg;

    while (running) {
        int pid = 0;
        pthread_mutex_lock(&discharge_mutex);
        while (running && discharge_count[bed_index] == 0)
            pthread_cond_wait(&discharge_conds[bed_index], &discharge_mutex);
        if (!running) {
            pthread_mutex_unlock(&discharge_mutex);
            break;
        }
        pthread_mutex_unlock(&discharge_mutex);
        if (!dequeue_discharge(bed_index, &pid))
            continue;

        printf("[Nurse-%s] Discharge signal received for patient %d\n", BED_LABELS[bed_index], pid);
        free_bed(pid);
    }

    return NULL;
}

void *discharge_listener_thread(void *arg) {
    (void)arg;

    while (running) {
        int fd = open(DISCHARGE_FIFO, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (!running) break;
            sleep(1);
            continue;
        }

        char buf[256];
        while (running) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *line = strtok(buf, "\n");
                while (line) {
                    int pid = atoi(line);
                    if (pid > 0) {
                        int type_index = lookup_patient_bed_type_index(pid);
                        enqueue_discharge(type_index, pid);
                        printf("[Discharge Listener] Routed patient %d to %s nurse\n",
                               pid, BED_LABELS[type_index]);
                    }
                    line = strtok(NULL, "\n");
                }
            } else {
                usleep(500000);
            }
        }
        close(fd);
    }

    pthread_mutex_lock(&discharge_mutex);
    for (int i = 0; i < 3; i++)
        pthread_cond_broadcast(&discharge_conds[i]);
    pthread_mutex_unlock(&discharge_mutex);
    return NULL;
}

/* ---- Free Bed + Coalesce ---- */
void free_bed(int patient_id) {
    pthread_mutex_lock(&bed_mutex);

    int found = -1;
    char bed_type[16] = {0};

    for (int i = 0; i < ward->num_partitions; i++) {
        if (!ward->partitions[i].is_free &&
            ward->partitions[i].patient_id == patient_id) {
            found = i;
            strncpy(bed_type, ward->partitions[i].bed_type, 15);
            break;
        }
    }

    if (found == -1) {
        pthread_mutex_unlock(&bed_mutex);
        return;
    }

    ward->partitions[found].is_free = 1;
    ward->partitions[found].patient_id = -1;
    ward->active_patients--;
    ward->total_patients_served++;

    /* Coalesce with NEXT partition */
    if (found + 1 < ward->num_partitions &&
        ward->partitions[found + 1].is_free &&
        strcmp(ward->partitions[found].bed_type, ward->partitions[found + 1].bed_type) == 0) {
        ward->partitions[found].size += ward->partitions[found + 1].size;
        for (int i = found + 1; i < ward->num_partitions - 1; i++)
            ward->partitions[i] = ward->partitions[i + 1];
        ward->num_partitions--;
    }

    /* Coalesce with PREVIOUS partition */
    if (found > 0 &&
        ward->partitions[found - 1].is_free &&
        strcmp(ward->partitions[found].bed_type, ward->partitions[found - 1].bed_type) == 0) {
        ward->partitions[found - 1].size += ward->partitions[found].size;
        for (int i = found; i < ward->num_partitions - 1; i++)
            ward->partitions[i] = ward->partitions[i + 1];
        ward->num_partitions--;
    }

    /* Post semaphore to allow next patient of this type */
    if (strcmp(bed_type, "ICU") == 0 && sem_icu)
        sem_post(sem_icu);
    else if (strcmp(bed_type, "ISOLATION") == 0 && sem_iso)
        sem_post(sem_iso);

    pthread_cond_broadcast(&bed_freed_cond);
    pthread_mutex_unlock(&bed_mutex);

    printf("[Admissions] Bed freed for patient %d (%s). Active: %d\n",
        patient_id, bed_type, ward->active_patients);
}

/* ---- Parse FIFO Record ---- */
PatientRecord parse_record(const char *line) {
    PatientRecord p;
    memset(&p, 0, sizeof(p));

    char buf[512];
    strncpy(buf, line, sizeof(buf) - 1);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    /* Format: ID|NAME|AGE|SEVERITY|PRIORITY|CARE_UNITS|BED_TYPE|TIMESTAMP */
    char *tok;
    tok = strtok(buf, "|"); if (tok) p.patient_id = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) strncpy(p.name, tok, MAX_NAME_LEN - 1);
    tok = strtok(NULL, "|"); if (tok) p.age = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) p.severity = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) p.priority = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) p.care_units = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) strncpy(p.bed_type, tok, 15);
    tok = strtok(NULL, "|"); /* timestamp - use current time */
    p.arrival_time = time(NULL);

    return p;
}

/* ---- Print Ward Status ---- */
void print_ward(void) {
    pthread_mutex_lock(&bed_mutex);
    printf("\n--- Ward Status ---\n");
    for (int i = 0; i < ward->num_partitions; i++) {
        printf("  [%d] %s | start:%d size:%d | %s",
               i, ward->partitions[i].bed_type,
               ward->partitions[i].start_unit,
               ward->partitions[i].size,
               ward->partitions[i].is_free ? "FREE" : "OCCUPIED");
        if (!ward->partitions[i].is_free)
            printf(" (patient %d)", ward->partitions[i].patient_id);
        printf("\n");
    }
    printf("  Active: %d | Served: %d\n-------------------\n\n",
           ward->active_patients, ward->total_patients_served);
    pthread_mutex_unlock(&bed_mutex);
}

/* ---- Discharge Thread ---- */
void *discharge_thread(void *arg) {
    (void)arg;

    while (running) {
        int fd = open(DISCHARGE_FIFO, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (!running) break;
            sleep(1);
            continue;
        }

        char buf[256];
        while (running) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                /* May contain multiple IDs separated by newlines */
                char *line = strtok(buf, "\n");
                while (line) {
                    int pid = atoi(line);
                    if (pid > 0) {
                        printf("[Discharge] Patient %d treatment complete.\n", pid);
                        free_bed(pid);
                        print_ward();
                    }
                    line = strtok(NULL, "\n");
                }
            } else {
                usleep(500000); /* 0.5 sec poll interval */
            }
        }
        close(fd);
    }
    return NULL;
}

/* ---- Fork + Exec Patient Simulator ---- */
void spawn_patient(PatientRecord *p, int bed_idx) {
    pid_t child = fork();

    if (child < 0) {
        perror("[Admissions] fork failed");
        return;
    }

    if (child == 0) {
        /* Child process: exec into patient_simulator */
        char id_str[16], pri_str[16], bed_str[16];
        snprintf(id_str, sizeof(id_str), "%d", p->patient_id);
        snprintf(pri_str, sizeof(pri_str), "%d", p->priority);
        snprintf(bed_str, sizeof(bed_str), "%d", bed_idx);

        char *args[] = {
            "./patient_simulator",
            id_str,
            p->name,
            pri_str,
            bed_str,
            p->bed_type,
            NULL
        };

        execv("./patient_simulator", args);
        perror("[Patient] execv failed");
        exit(1);
    }

    /* Parent continues */
    printf("[Admissions] Forked patient process PID %d for %s (bed %d, %s)\n",
           child, p->name, bed_idx, p->bed_type);
}

/* ---- Cleanup ---- */
void cleanup(void) {
    if (ward && shm_id >= 0) {
        shmdt(ward);
        shmctl(shm_id, IPC_RMID, NULL);
    }
    if (sem_icu) { sem_close(sem_icu); sem_unlink(SEM_ICU_LIMIT); }
    if (sem_iso) { sem_close(sem_iso); sem_unlink(SEM_ISO_LIMIT); }
    sem_destroy(&queue_slots_sem);
    sem_destroy(&queue_items_sem);
    pthread_mutex_destroy(&bed_mutex);
    pthread_mutex_destroy(&queue_mutex);
    pthread_mutex_destroy(&discharge_mutex);
    pthread_cond_destroy(&queue_not_empty);
    pthread_cond_destroy(&queue_not_full);
    pthread_cond_destroy(&bed_freed_cond);
    for (int i = 0; i < 3; i++)
        pthread_cond_destroy(&discharge_conds[i]);
}

/* ---- Main ---- */
int main(void) {

    /* Register signal handlers */
    signal(SIGTERM, handle_sigterm);
    signal(SIGCHLD, handle_sigchld);

    /* Create shared memory */
    shm_id = shmget(SHM_KEY, sizeof(WardStatus), IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("[Admissions] shmget failed");
        return 1;
    }
    ward = (WardStatus *)shmat(shm_id, NULL, 0);
    if (ward == (void *)-1) {
        perror("[Admissions] shmat failed");
        return 1;
    }

    memset(discharge_queue, 0, sizeof(discharge_queue));
    memset(discharge_count, 0, sizeof(discharge_count));

    /* Initialize ward */
    init_ward();

    if (sem_init(&queue_slots_sem, 0, MAX_QUEUE_SIZE) != 0) {
        perror("[Admissions] sem_init(queue_slots) failed");
        cleanup();
        return 1;
    }
    if (sem_init(&queue_items_sem, 0, 0) != 0) {
        perror("[Admissions] sem_init(queue_items) failed");
        cleanup();
        return 1;
    }

    /* Create semaphores */
    sem_unlink(SEM_ICU_LIMIT);
    sem_unlink(SEM_ISO_LIMIT);
    sem_icu = sem_open(SEM_ICU_LIMIT, O_CREAT, 0666, ICU_CAPACITY);
    sem_iso = sem_open(SEM_ISO_LIMIT, O_CREAT, 0666, ISOLATION_CAPACITY);

    /* Startup banner */
    printf("============================================\n");
    printf("  Hospital Admissions Manager Started\n");
    printf("  PID: %d\n", getpid());
    printf("  ICU Beds:       %d (%d units each)\n", ICU_BEDS, ICU_CARE_UNITS);
    printf("  Isolation Beds: %d (%d units each)\n", ISOLATION_BEDS, ISOLATION_CARE_UNITS);
    printf("  General Beds:   %d (%d unit each)\n", GENERAL_BEDS, GENERAL_CARE_UNITS);
    printf("  Total Units:    %d\n", TOTAL_WARD_UNITS);
    printf("============================================\n");
    printf("  Waiting for patients...\n\n");
    fflush(stdout);

    pthread_t receptionist_tid, scheduler_tid, discharge_tid;
    pthread_t nurse_tids[3];
    int nurse_roles[3] = {BED_ICU, BED_ISOLATION, BED_GENERAL};

    pthread_create(&receptionist_tid, NULL, receptionist_thread, NULL);
    pthread_create(&scheduler_tid, NULL, scheduler_thread, NULL);
    pthread_create(&discharge_tid, NULL, discharge_listener_thread, NULL);
    for (int i = 0; i < 3; i++)
        pthread_create(&nurse_tids[i], NULL, nurse_thread, &nurse_roles[i]);

    pthread_join(receptionist_tid, NULL);
    pthread_join(scheduler_tid, NULL);
    pthread_join(discharge_tid, NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(nurse_tids[i], NULL);

    /* Shutdown */
    printf("\n============================================\n");
    printf("  Admissions Manager Shutting Down...\n");
    printf("  Total Patients Served: %d\n", ward->total_patients_served);
    printf("  Active Patients: %d\n", ward->active_patients);
    printf("============================================\n");

    running = 0;
    cleanup();

    return 0;
}
