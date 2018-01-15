/*
 * Copyright (c) 2017      Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2017      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_TIMING_H
#define PMIX_UTIL_TIMING_H

#if (PMIX_ENABLE_TIMING)

typedef double (*pmix_timing_ts_func_t)(void);

typedef enum {
    PMIX_TIMING_AUTOMATIC_TIMER,
    PMIX_TIMING_GET_TIME_OF_DAY,
    PMIX_TIMING_CYCLE_NATIVE,
    PMIX_TIMING_USEC_NATIVE
} pmix_timer_type_t;

#define PMIX_TIMING_STR_LEN 256

typedef struct {
    char desc[PMIX_TIMING_STR_LEN];
    double ts;
    char *file;
    char *prefix;
}   pmix_timing_val_t;

typedef struct {
    pmix_timing_val_t *val;
    int use;
    struct pmix_timing_list_t *next;
} pmix_timing_list_t;

typedef struct pmix_timing_t {
    double ts;
    const char *prefix;
    int size;
    int cnt;
    int error;
    int enabled;
    pmix_timing_ts_func_t get_ts;
    pmix_timing_list_t *timing;
    pmix_timing_list_t *cur_timing;
} pmix_timing_t;

#define PMIX_TIMING_INIT(_size)                                                \
    pmix_timing_t PMIX_TIMING;                                                 \
    PMIX_TIMING.prefix = __func__;                                             \
    PMIX_TIMING.size = _size;                                                  \
    PMIX_TIMING.get_ts = pmix_timing_ts_func(PMIX_TIMING_AUTOMATIC_TIMER);     \
    PMIX_TIMING.cnt = 0;                                                       \
    PMIX_TIMING.error = 0;                                                     \
    PMIX_TIMING.ts = PMIX_TIMING.get_ts();                                     \
    PMIX_TIMING.enabled = 0;                                                   \
    {                                                                          \
        char *ptr;                                                             \
        ptr = getenv("PMIX_TIMING_ENABLE");                                    \
        if (NULL != ptr) {                                                     \
            PMIX_TIMING.enabled = atoi(ptr);                                   \
        }                                                                      \
        if (PMIX_TIMING.enabled) {                                             \
            setenv("PMIX_TIMING_ENABLE", "1", 1);                              \
            PMIX_TIMING.timing = (pmix_timing_list_t*)malloc(sizeof(pmix_timing_list_t));              \
            memset(PMIX_TIMING.timing, 0, sizeof(pmix_timing_list_t));         \
            PMIX_TIMING.timing->val = (pmix_timing_val_t*)malloc(sizeof(pmix_timing_val_t) * _size);   \
            PMIX_TIMING.cur_timing = PMIX_TIMING.timing;                       \
        }                                                                      \
    }

#define PMIX_TIMING_ITEM_EXTEND                                                    \
    do {                                                                           \
        if (PMIX_TIMING.enabled) {                                                 \
            PMIX_TIMING.cur_timing->next = (struct pmix_timing_list_t*)malloc(sizeof(pmix_timing_list_t)); \
            PMIX_TIMING.cur_timing = (pmix_timing_list_t*)PMIX_TIMING.cur_timing->next;                    \
            memset(PMIX_TIMING.cur_timing, 0, sizeof(pmix_timing_list_t));                                 \
            PMIX_TIMING.cur_timing->val = malloc(sizeof(pmix_timing_val_t) * PMIX_TIMING.size);            \
        }                                                                          \
    } while(0)

#define PMIX_TIMING_FINALIZE                                                       \
    do {                                                                           \
        if (PMIX_TIMING.enabled) {                                                 \
            pmix_timing_list_t *t = PMIX_TIMING.timing, *tmp;                      \
            while ( NULL != t) {                                                   \
                tmp = t;                                                           \
                t = (pmix_timing_list_t*)t->next;                                  \
                free(tmp->val);                                                    \
                free(tmp);                                                         \
            }                                                                      \
            PMIX_TIMING.timing = NULL;                                             \
            PMIX_TIMING.cur_timing = NULL;                                         \
            PMIX_TIMING.cnt = 0;                                                   \
        }                                                                          \
    } while(0)

#define PMIX_TIMING_NEXT(...)                                                      \
    do {                                                                           \
        if (!PMIX_TIMING.error && PMIX_TIMING.enabled) {                           \
            char *f = strrchr(__FILE__, '/') + 1;                                  \
            int len = 0;                                                           \
            if (PMIX_TIMING.cur_timing->use >= PMIX_TIMING.size){                  \
                PMIX_TIMING_ITEM_EXTEND;                                           \
            }                                                                      \
            len = snprintf(PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].desc,        \
                PMIX_TIMING_STR_LEN, ##__VA_ARGS__);                               \
            if (len >= PMIX_TIMING_STR_LEN) {                                      \
                PMIX_TIMING.error = 1;                                             \
            }                                                                      \
            PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].file = strdup(f);     \
            PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].prefix = strdup(__func__);      \
            PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use++].ts =        \
                PMIX_TIMING.get_ts() - PMIX_TIMING.ts;                             \
            PMIX_TIMING.cnt++;                                                     \
            PMIX_TIMING.ts = PMIX_TIMING.get_ts();                                 \
        }                                                                          \
    } while(0)

#define PMIX_TIMING_APPEND(filename,func,desc,ts)                                  \
    do {                                                                           \
        if (PMIX_TIMING.cur_timing->use >= PMIX_TIMING.size){                      \
            PMIX_TIMING_ITEM_EXTEND;                                               \
        }                                                                          \
        int len = snprintf(PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].desc,        \
            PMIX_TIMING_STR_LEN, "%s", desc);                                      \
        if (len >= PMIX_TIMING_STR_LEN) {                                          \
            PMIX_TIMING.error = 1;                                                 \
        }                                                                          \
        PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].prefix = func;    \
        PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use].file = filename;  \
        PMIX_TIMING.cur_timing->val[PMIX_TIMING.cur_timing->use++].ts = ts;        \
        PMIX_TIMING.cnt++;                                                         \
    } while(0)

#define PMIX_TIMING_IMPORT_PMIX_PREFIX(_prefix, func)                              \
    do {                                                                           \
        if (!PMIX_TIMING.error && PMIX_TIMING.enabled) {                           \
            int cnt;                                                               \
            int i;                                                                 \
            double ts;                                                             \
            PMIX_TIMING_ENV_CNT(func, cnt);                                        \
            PMIX_TIMING_ENV_ERROR_PREFIX(_prefix, func, PMIX_TIMING.error);        \
            for(i = 0; i < cnt; i++){                                              \
                char *desc, *filename;                                             \
                PMIX_TIMING_ENV_GETDESC_PREFIX(_prefix, &filename, func, i, &desc, ts);  \
                PMIX_TIMING_APPEND(filename, func, desc, ts);                      \
            }                                                                      \
        }                                                                          \
    } while(0)

#define PMIX_TIMING_IMPORT_OPAL(func)                                              \
        PMIX_TIMING_IMPORT_PMIX_PREFIX("", func);

#define PMIX_TIMING_OUT                                                           \
    do {                                                                          \
        if (PMIX_TIMING.enabled) {                                                \
            int i, size, rank;                                                    \
            MPI_Comm_size(MPI_COMM_WORLD, &size);                                 \
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);                                 \
            int error = 0;                                                        \
                                                                                  \
            MPI_Reduce(&PMIX_TIMING.error, &error, 1,                             \
                MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);                             \
                                                                                  \
            if (error) {                                                          \
                if (0 == rank) {                                                  \
                    printf("==PMIX_TIMING== error: something went wrong, timings doesn't work\n"); \
                }                                                                 \
            }                                                                     \
            else {                                                                \
                double *avg = (double*)malloc(sizeof(double) * PMIX_TIMING.cnt);  \
                double *min = (double*)malloc(sizeof(double) * PMIX_TIMING.cnt);  \
                double *max = (double*)malloc(sizeof(double) * PMIX_TIMING.cnt);  \
                char **desc = (char**)malloc(sizeof(char*) * PMIX_TIMING.cnt);    \
                char **prefix = (char**)malloc(sizeof(char*) * PMIX_TIMING.cnt);  \
                char **file = (char**)malloc(sizeof(char*) * PMIX_TIMING.cnt);    \
                                                                                  \
                if( PMIX_TIMING.cnt > 0 ) {                                       \
                    PMIX_TIMING.ts = PMIX_TIMING.get_ts();                        \
                    pmix_timing_list_t *timing = PMIX_TIMING.timing;              \
                    i = 0;                                                        \
                    do {                                                          \
                        int use;                                                  \
                        for (use = 0; use < timing->use; use++) {                 \
                            MPI_Reduce(&timing->val[use].ts, avg + i, 1,          \
                                MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);          \
                            MPI_Reduce(&timing->val[use].ts, min + i, 1,          \
                                MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);          \
                            MPI_Reduce(&timing->val[use].ts, max + i, 1,          \
                                MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);          \
                            desc[i] = timing->val[use].desc;                      \
                            prefix[i] = timing->val[use].prefix;                  \
                            file[i] = timing->val[use].file;                      \
                            i++;                                                  \
                        }                                                         \
                        timing = (pmix_timing_list_t*)timing->next;               \
                    } while (timing != NULL);                                     \
                                                                                  \
                    if( 0 == rank ){                                              \
                        if (PMIX_TIMING.timing->next) {                           \
                            printf("==PMIX_TIMING== warning: added the extra timings allocation that might misrepresent the results.\n"            \
                                   "==PMIX_TIMING==          Increase the inited size of timings to avoid extra allocation during runtime.\n");    \
                        }                                                         \
                                                                                  \
                        printf("------------------ %s ------------------\n",      \
                                PMIX_TIMING.prefix);                              \
                        for(i=0; i< PMIX_TIMING.cnt; i++){                        \
                            avg[i] /= size;                                       \
                            printf("[%s:%s:%s]: %lf / %lf / %lf\n",               \
                                file[i], prefix[i], desc[i], avg[i], min[i], max[i]); \
                        }                                                         \
                        printf("[%s:overhead]: %lf \n", PMIX_TIMING.prefix,       \
                                PMIX_TIMING.get_ts() - PMIX_TIMING.ts);           \
                    }                                                             \
                }                                                                 \
                free(avg);                                                        \
                free(min);                                                        \
                free(max);                                                        \
                free(desc);                                                       \
                free(prefix);                                                     \
                free(file);                                                       \
            }                                                                     \
        }                                                                         \
    } while(0)

#else
#define PMIX_TIMING_INIT(size)

#define PMIX_TIMING_NEXT(...)

#define PMIX_TIMING_APPEND(desc,ts)

#define PMIX_TIMING_OUT

#define PMIX_TIMING_IMPORT_OPAL(func)

#define PMIX_TIMING_FINALIZE

#endif

#endif
