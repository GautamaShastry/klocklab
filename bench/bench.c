#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../common/klocklab.h"

/* ---- Workload distribution types ---- */
enum dist_type {
    DIST_UNIFORM = 0,
    DIST_HOTSPOT = 1,
    DIST_ZIPFIAN = 2,
};

/* ---- Burst pattern ---- */
enum burst_type {
    BURST_NONE    = 0,  /* constant rate */
    BURST_PERIODIC = 1, /* burst of ops then pause */
};

struct thread_result {
    uint64_t latency_min_ns;
    uint64_t latency_max_ns;
    uint64_t latency_sum_ns;
    uint64_t ops_done;
};

struct thread_args {
    int tid;
    int fd;
    uint64_t ops;
    uint32_t key_space;
    uint32_t hot_key;
    uint32_t hotspot_pct;
    unsigned int seed;
    int pin_cpu;
    /* New fields */
    int dist;            /* distribution type */
    double zipf_alpha;   /* Zipfian skew parameter */
    double *zipf_cdf;    /* precomputed CDF for Zipfian */
    int read_pct;        /* percentage of ops that are reads (0-100) */
    int burst_type;
    uint32_t burst_size; /* ops per burst */
    uint32_t burst_pause_us; /* microseconds pause between bursts */
    struct thread_result result;
};

static inline uint64_t nsec_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_to_cpu_if_requested(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        perror("pthread_setaffinity_np");
}

/* ---- Zipfian distribution ---- */

static double *zipf_build_cdf(uint32_t n, double alpha)
{
    double *cdf = calloc(n, sizeof(double));
    double sum = 0.0;
    uint32_t i;

    if (!cdf) return NULL;

    for (i = 1; i <= n; i++)
        sum += 1.0 / pow((double)i, alpha);

    double running = 0.0;
    for (i = 0; i < n; i++) {
        running += 1.0 / pow((double)(i + 1), alpha);
        cdf[i] = running / sum;
    }
    return cdf;
}

static uint32_t zipf_sample(double *cdf, uint32_t n, unsigned int *seed)
{
    double u = (double)rand_r(seed) / (double)RAND_MAX;
    /* Binary search for the bucket */
    uint32_t lo = 0, hi = n - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cdf[mid] < u)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ---- Key selection ---- */

static uint32_t choose_key(struct thread_args *a)
{
    switch (a->dist) {
    case DIST_ZIPFIAN:
        return zipf_sample(a->zipf_cdf, a->key_space, &a->seed);

    case DIST_HOTSPOT: {
        uint32_t r = rand_r(&a->seed) % 100;
        if (r < a->hotspot_pct)
            return a->hot_key;
        return rand_r(&a->seed) % a->key_space;
    }

    case DIST_UNIFORM:
    default:
        return rand_r(&a->seed) % a->key_space;
    }
}

static int choose_op_type(struct thread_args *a)
{
    if (a->read_pct <= 0) return KLOCKLAB_OP_WRITE;
    if (a->read_pct >= 100) return KLOCKLAB_OP_READ;
    return ((rand_r(&a->seed) % 100) < (uint32_t)a->read_pct)
           ? KLOCKLAB_OP_READ : KLOCKLAB_OP_WRITE;
}

static void *worker(void *arg)
{
    struct thread_args *a = (struct thread_args *)arg;
    struct klocklab_update_req req;
    uint64_t i;
    uint64_t lat_min = UINT64_MAX, lat_max = 0, lat_sum = 0;

    pin_to_cpu_if_requested(a->pin_cpu);

    for (i = 0; i < a->ops; i++) {
        uint64_t t0, t1, dt;

        /* Burst pattern: pause after every burst_size ops */
        if (a->burst_type == BURST_PERIODIC &&
            a->burst_size > 0 &&
            i > 0 && (i % a->burst_size) == 0) {
            usleep(a->burst_pause_us);
        }

        req.key = choose_key(a);
        req.op_type = choose_op_type(a);

        t0 = nsec_now();
        if (ioctl(a->fd, KLOCKLAB_IOCTL_UPDATE, &req) != 0) {
            perror("ioctl(KLOCKLAB_IOCTL_UPDATE)");
            return (void *)1;
        }
        t1 = nsec_now();
        dt = t1 - t0;

        lat_sum += dt;
        if (dt < lat_min) lat_min = dt;
        if (dt > lat_max) lat_max = dt;
    }

    a->result.latency_min_ns = lat_min;
    a->result.latency_max_ns = lat_max;
    a->result.latency_sum_ns = lat_sum;
    a->result.ops_done = a->ops;
    return 0;
}

static const char *mode_name(uint32_t m)
{
    switch (m) {
    case KLOCKLAB_MODE_GLOBAL:  return "global";
    case KLOCKLAB_MODE_SHARDED: return "sharded";
    case KLOCKLAB_MODE_PERCPU:  return "percpu";
    case KLOCKLAB_MODE_RCU:     return "rcu";
    case KLOCKLAB_MODE_ATOMIC:  return "atomic";
    default:                    return "unknown";
    }
}

static const char *dist_name(int d)
{
    switch (d) {
    case DIST_UNIFORM:  return "uniform";
    case DIST_HOTSPOT:  return "hotspot";
    case DIST_ZIPFIAN:  return "zipfian";
    default:            return "unknown";
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -t <threads>         number of worker threads (default: 4)\n"
        "  -o <ops_per_thread>  operations per thread (default: 200000)\n"
        "  -k <key_space>       key range [0, key_space) (default: 1024)\n"
        "  -d <distribution>    uniform|hotspot|zipfian (default: hotspot)\n"
        "  -H <hotspot_pct>     hotspot percentage 0-100 (default: 90)\n"
        "  -z <zipf_alpha>      Zipfian skew, >0 (default: 1.2)\n"
        "  -r <read_pct>        read percentage 0-100 (default: 0)\n"
        "  -b <burst_size>      ops per burst, 0=no burst (default: 0)\n"
        "  -p <burst_pause_us>  pause between bursts in us (default: 100)\n"
        "  -P                   pin threads to CPUs\n"
        "  -h                   show this help\n",
        prog);
}

int main(int argc, char **argv)
{
    int fd, i, opt;
    int threads = 4;
    uint64_t ops_per_thread = 200000;
    uint32_t key_space = 1024;
    int dist = DIST_HOTSPOT;
    uint32_t hotspot_pct = 90;
    double zipf_alpha = 1.2;
    int read_pct = 0;
    int pin = 0;
    int burst = BURST_NONE;
    uint32_t burst_size = 0;
    uint32_t burst_pause_us = 100;

    pthread_t *tids;
    struct thread_args *args;
    double *zipf_cdf = NULL;
    uint64_t start_ns, end_ns, total_ops;
    double seconds, ops_per_sec;
    uint64_t agg_lat_min = UINT64_MAX, agg_lat_max = 0, agg_lat_sum = 0;
    struct klocklab_stats st;

    while ((opt = getopt(argc, argv, "t:o:k:d:H:z:r:b:p:Ph")) != -1) {
        switch (opt) {
        case 't': threads = atoi(optarg); break;
        case 'o': ops_per_thread = strtoull(optarg, NULL, 10); break;
        case 'k': key_space = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'd':
            if (strcmp(optarg, "uniform") == 0) dist = DIST_UNIFORM;
            else if (strcmp(optarg, "hotspot") == 0) dist = DIST_HOTSPOT;
            else if (strcmp(optarg, "zipfian") == 0) dist = DIST_ZIPFIAN;
            else { fprintf(stderr, "Unknown dist: %s\n", optarg); return 1; }
            break;
        case 'H': hotspot_pct = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'z': zipf_alpha = atof(optarg); break;
        case 'r': read_pct = atoi(optarg); break;
        case 'b':
            burst_size = (uint32_t)strtoul(optarg, NULL, 10);
            if (burst_size > 0) burst = BURST_PERIODIC;
            break;
        case 'p': burst_pause_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'P': pin = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (threads <= 0 || key_space == 0) {
        usage(argv[0]);
        return 1;
    }

    /* Build Zipfian CDF if needed */
    if (dist == DIST_ZIPFIAN) {
        zipf_cdf = zipf_build_cdf(key_space, zipf_alpha);
        if (!zipf_cdf) {
            perror("zipf_build_cdf");
            return 1;
        }
    }

    fd = open("/dev/klocklab", O_RDWR);
    if (fd < 0) {
        perror("open(/dev/klocklab)");
        free(zipf_cdf);
        return 1;
    }

    if (ioctl(fd, KLOCKLAB_IOCTL_RESET) != 0) {
        perror("ioctl(KLOCKLAB_IOCTL_RESET)");
        close(fd);
        free(zipf_cdf);
        return 1;
    }

    tids = calloc(threads, sizeof(*tids));
    args = calloc(threads, sizeof(*args));
    if (!tids || !args) {
        perror("calloc");
        close(fd);
        free(zipf_cdf);
        return 1;
    }

    start_ns = nsec_now();

    for (i = 0; i < threads; i++) {
        args[i].tid = i;
        args[i].fd = fd;
        args[i].ops = ops_per_thread;
        args[i].key_space = key_space;
        args[i].hot_key = 0;
        args[i].hotspot_pct = hotspot_pct;
        args[i].seed = (unsigned int)(0x1234u + i * 7919u);
        args[i].pin_cpu = pin ? (i % sysconf(_SC_NPROCESSORS_ONLN)) : -1;
        args[i].dist = dist;
        args[i].zipf_alpha = zipf_alpha;
        args[i].zipf_cdf = zipf_cdf;
        args[i].read_pct = read_pct;
        args[i].burst_type = burst;
        args[i].burst_size = burst_size;
        args[i].burst_pause_us = burst_pause_us;

        if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0) {
            perror("pthread_create");
            close(fd);
            free(tids);
            free(args);
            free(zipf_cdf);
            return 1;
        }
    }

    for (i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(tids[i], &ret);
        if (ret != 0) {
            fprintf(stderr, "worker %d failed\n", i);
            close(fd);
            free(tids);
            free(args);
            free(zipf_cdf);
            return 1;
        }
        /* Aggregate per-thread latency */
        if (args[i].result.latency_min_ns < agg_lat_min)
            agg_lat_min = args[i].result.latency_min_ns;
        if (args[i].result.latency_max_ns > agg_lat_max)
            agg_lat_max = args[i].result.latency_max_ns;
        agg_lat_sum += args[i].result.latency_sum_ns;
    }

    end_ns = nsec_now();

    if (ioctl(fd, KLOCKLAB_IOCTL_GET_STATS, &st) != 0) {
        perror("ioctl(KLOCKLAB_IOCTL_GET_STATS)");
        close(fd);
        free(tids);
        free(args);
        free(zipf_cdf);
        return 1;
    }

    total_ops = (uint64_t)threads * ops_per_thread;
    seconds = (double)(end_ns - start_ns) / 1e9;
    ops_per_sec = (double)total_ops / seconds;

    printf("=== klocklab benchmark ===\n");
    printf("threads           : %d\n", threads);
    printf("ops_per_thread    : %llu\n", (unsigned long long)ops_per_thread);
    printf("total_ops         : %llu\n", (unsigned long long)total_ops);
    printf("key_space         : %u\n", key_space);
    printf("distribution      : %s\n", dist_name(dist));
    if (dist == DIST_HOTSPOT)
        printf("hotspot_pct       : %u\n", hotspot_pct);
    if (dist == DIST_ZIPFIAN)
        printf("zipf_alpha        : %.2f\n", zipf_alpha);
    printf("read_pct          : %d\n", read_pct);
    printf("burst             : %s\n", burst == BURST_PERIODIC ? "periodic" : "none");
    if (burst == BURST_PERIODIC) {
        printf("burst_size        : %u\n", burst_size);
        printf("burst_pause_us    : %u\n", burst_pause_us);
    }
    printf("thread_pinning    : %s\n", pin ? "on" : "off");
    printf("elapsed_sec       : %.6f\n", seconds);
    printf("throughput_ops/s  : %.2f\n", ops_per_sec);
    printf("\n");

    printf("=== userspace latency (ioctl round-trip) ===\n");
    printf("lat_min_ns        : %llu\n", (unsigned long long)agg_lat_min);
    printf("lat_max_ns        : %llu\n", (unsigned long long)agg_lat_max);
    printf("lat_avg_ns        : %llu\n", (unsigned long long)(agg_lat_sum / total_ops));
    printf("\n");

    printf("=== kernel stats ===\n");
    printf("mode              : %s (%u)\n", mode_name(st.mode), st.mode);
    printf("num_keys          : %u\n", st.num_keys);
    printf("num_shards        : %u\n", st.num_shards);
    printf("nr_cpus_seen      : %u\n", st.nr_cpus_seen);
    printf("total_writes      : %llu\n", (unsigned long long)st.total_updates);
    printf("total_reads       : %llu\n", (unsigned long long)st.total_reads);
    printf("total_sum         : %llu\n", (unsigned long long)st.total_sum);
    printf("invalid_requests  : %llu\n", (unsigned long long)st.invalid_requests);
    printf("kern_lat_min_ns   : %llu\n", (unsigned long long)st.latency_min_ns);
    printf("kern_lat_max_ns   : %llu\n", (unsigned long long)st.latency_max_ns);
    if (total_ops > 0)
        printf("kern_lat_avg_ns   : %llu\n",
               (unsigned long long)(st.latency_total_ns / total_ops));
    printf("sample[0..7]      : ");
    for (i = 0; i < 8; i++)
        printf("%llu%s", (unsigned long long)st.sample[i], (i == 7) ? "\n" : ",");

    close(fd);
    free(tids);
    free(args);
    free(zipf_cdf);
    return 0;
}
