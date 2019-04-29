#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mpi.h"
#include <time.h>
#include <unistd.h>
#include <pwd.h>

inline static double get_time_nsec()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (ts.tv_sec + 1E-9 * ts.tv_nsec);
}

int main(int argc, char* argv[])
{
    int rank, size, len;
    char version[MPI_MAX_LIBRARY_VERSION_STRING];
    char hostname[256], *fname, *dir, *jobid_str = NULL;
    uid_t uid = geteuid();
    struct passwd *pw = getpwuid(uid);
    struct stat st = {0};
    
    rank = atoi(getenv("PMIX_RANK"));
    jobid_str = getenv("SLURM_JOB_ID");
    
    gethostname(hostname, sizeof(hostname));
    if (NULL == jobid_str) {
        asprintf(&dir, "/tmp/%s", pw->pw_name);
    } else {
        asprintf(&dir, "/tmp/%s_%s", pw->pw_name, jobid_str);
    }

    if (stat(dir, &st) == -1) {
        mkdir(dir, 0700);
    }
    
    asprintf(&fname, "%s/mpi_out_%s_%0.4d.log", dir, hostname, rank);
    
    fclose(stderr);
    stderr = fopen(fname, "w");

    fprintf(stderr, "[%s:%d] app/main %lf\n", hostname, getpid(), get_time_nsec());
    MPI_Init(&argc, &argv);
    fprintf(stderr, "[%s:%d] app/mpi_init_done %lf\n", hostname, getpid(), get_time_nsec());

    free(fname);
    fclose(stderr);
    
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Get_library_version(version, &len);
    
    MPI_Finalize();

    return 0;
}
