/**
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *               2012  Zheng <zpwu@uwaterloo.ca>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/* clang -S -mllvm --x86-asm-syntax=intel ./bandwidth.c */

/* bandwidth with L2 monitor reporting: with -M (or -R), snapshot the LLC
 * monitor's per-RCID counters around the timed run and print the deltas. */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/

/**************************************************************************
 * Included Files
 **************************************************************************/
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64	   /* cache Line size is 64 byte */
#define DEFAULT_ALLOC_SIZE_KB 16384

/* L2 monitor: one 0x80-byte block per RCID, cycle counter at 0x800 */
#define MON_BASE        0x21001000UL
#define MON_SIZE        0x1000UL
#define MON_RCID_STRIDE 0x80
#define MON_CYCLES      0x800
#define MON_NRCID       4          /* nRCID of CBQRIQuadBoomConfig */

/**************************************************************************
 * Public Types
 **************************************************************************/
enum access_type { READ, WRITE};
enum { CLEAN_LAT, CLEAN_CNT, CONT_LAT, CONT_CNT, SVC_LAT, SVC_CNT, STALL_MSHR, WANT, NCTR };

struct mon_snap {
	uint64_t c[MON_NRCID][NCTR];
	uint64_t cycles;
};

/**************************************************************************
 * Global Variables
 **************************************************************************/
int64_t g_mem_size = DEFAULT_ALLOC_SIZE_KB * 1024;	   /* memory size */
int *g_mem_ptr = 0;		   /* pointer to allocated memory region */

volatile uint64_t g_nread = 0;	           /* number of bytes read */
volatile unsigned int g_start;		   /* starting time */
int cpuid = 0;
int g_stride = CACHE_LINE_SIZE;

volatile uint64_t *g_mon = NULL;	   /* mapped L2 monitor, NULL if not reporting */
int g_mon_rcids[MON_NRCID];		   /* RCIDs to report */
int g_mon_nrcids = 0;			   /* 0: all RCIDs with activity */
struct mon_snap g_mon_start;

/**************************************************************************
 * Public Functions
 **************************************************************************/
unsigned int get_usecs()
{
	struct timeval         time;
	gettimeofday(&time, NULL);
	return (time.tv_sec * 1000000 +	time.tv_usec);
}

unsigned long get_default_hugepage_size_kb()
{
	FILE *meminfo;
	char name[32];
	unsigned long size_kb;

	meminfo = fopen("/proc/meminfo", "r");
	if (meminfo == NULL)
		return 0;

	while (fscanf(meminfo, "%31s %lu kB", name, &size_kb) == 2) {
		if (!strcmp(name, "Hugepagesize:")) {
			fclose(meminfo);
			return size_kb;
		}
	}

	fclose(meminfo);
	return 0;
}

static int hugepage_order(unsigned long size_kb)
{
	uint64_t size = (uint64_t)size_kb * 1024;
	int order = 0;

	if (size == 0 || (size & (size - 1)) != 0)
		return -1;

	while (size > 1) {
		size >>= 1;
		order++;
	}
	return order;
}

static void *map_hugepage(size_t length, unsigned long *mapped_size_kb)
{
	DIR *hugepage_dir;
	struct dirent *entry;
	void *mapping = MAP_FAILED;
	unsigned long best_size_kb = 0;
	int saved_errno = ENOMEM;

	hugepage_dir = opendir("/sys/kernel/mm/hugepages");
	if (hugepage_dir != NULL) {
		while ((entry = readdir(hugepage_dir)) != NULL) {
			unsigned long size_kb;
			unsigned long free_pages;
			char free_path[PATH_MAX];
			FILE *free_file;
			int order;

			if (sscanf(entry->d_name, "hugepages-%lukB", &size_kb) != 1 ||
			    size_kb <= best_size_kb)
				continue;

			order = hugepage_order(size_kb);
			if (order < 0 || (uint64_t)size_kb * 1024 > length ||
			    length % ((uint64_t)size_kb * 1024) != 0)
				continue;

			snprintf(free_path, sizeof(free_path),
			         "/sys/kernel/mm/hugepages/%s/free_hugepages",
			         entry->d_name);
			free_file = fopen(free_path, "r");
			if (free_file == NULL ||
			    fscanf(free_file, "%lu", &free_pages) != 1) {
				if (free_file != NULL)
					fclose(free_file);
				continue;
			}
			fclose(free_file);
			if (free_pages == 0)
				continue;

			mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
			               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
			               (order << MAP_HUGE_SHIFT), -1, 0);
			if (mapping != MAP_FAILED) {
				best_size_kb = size_kb;
				*mapped_size_kb = size_kb;
				break;
			}
			saved_errno = errno;
		}
		closedir(hugepage_dir);
	}

	if (mapping == MAP_FAILED) {
		/* Preserve compatibility with kernels that do not expose hugetlbfs
		 * pools in sysfs. */
		mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (mapping != MAP_FAILED)
			*mapped_size_kb = get_default_hugepage_size_kb();
		else
			saved_errno = errno;
	}

	if (mapping == MAP_FAILED)
		errno = saved_errno;
	return mapping;
}

void mon_map()
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	void *p;

	if (fd < 0) {
		perror("open /dev/mem");
		exit(1);
	}
	p = mmap(NULL, MON_SIZE, PROT_READ, MAP_SHARED, fd, MON_BASE);
	close(fd);
	if (p == MAP_FAILED) {
		perror("mmap L2 monitor");
		exit(1);
	}
	g_mon = (volatile uint64_t *)p;
}

void mon_snapshot(struct mon_snap *s)
{
	int r, k;

	s->cycles = g_mon[MON_CYCLES / 8];
	for (r = 0; r < MON_NRCID; r++)
		for (k = 0; k < NCTR; k++)
			s->c[r][k] = g_mon[(MON_RCID_STRIDE * r) / 8 + k];
}

/* parse a comma-separated RCID list, e.g. "0,1" */
void mon_parse_rcids(char *arg)
{
	char *tok;

	g_mon_nrcids = 0;
	for (tok = strtok(arg, ","); tok != NULL; tok = strtok(NULL, ",")) {
		int r = strtol(tok, NULL, 0);
		if (r < 0 || r >= MON_NRCID || g_mon_nrcids >= MON_NRCID) {
			fprintf(stderr, "bad rcid list (rcids are 0..%d)\n", MON_NRCID - 1);
			exit(1);
		}
		g_mon_rcids[g_mon_nrcids++] = r;
	}
}

/* x / y as a percentage with two decimals */
static void pct(const char *name, uint64_t x, uint64_t y)
{
	uint64_t p = y ? x * 10000 / y : 0;
	printf(" %s=%" PRIu64 " (%" PRIu64 ".%02" PRIu64 "%%)", name, x, p / 100, p % 100);
}

static void avg(const char *name, uint64_t sum, uint64_t cnt)
{
	printf(" %s=%" PRIu64 "/%" PRIu64, name, cnt ? sum / cnt : 0, cnt);
}

void mon_report()
{
	struct mon_snap end;
	uint64_t d[NCTR];
	int i, k;

	mon_snapshot(&end);
	printf("l2mon: window %" PRIu64 " L2 cycles\n", end.cycles - g_mon_start.cycles);
	for (i = 0; i < (g_mon_nrcids ? g_mon_nrcids : MON_NRCID); i++) {
		int r = g_mon_nrcids ? g_mon_rcids[i] : i;

		for (k = 0; k < NCTR; k++)
			d[k] = end.c[r][k] - g_mon_start.c[r][k];
		/* with no list given, skip RCIDs that did nothing */
		if (!g_mon_nrcids && !d[WANT] && !d[SVC_CNT] && !d[CLEAN_CNT] && !d[CONT_CNT])
			continue;
		printf("  rcid %d:", r);
		avg("llc_svc", d[SVC_LAT], d[SVC_CNT]);
		printf(" want=%" PRIu64, d[WANT]);
		pct("stall_mshr", d[STALL_MSHR], d[WANT]);
		avg("dram_clean", d[CLEAN_LAT], d[CLEAN_CNT]);
		avg("dram_cont", d[CONT_LAT], d[CONT_CNT]);
		printf("\n");
	}
}

void quit(int param)
{
	float dur_in_sec;
	float bw;
	float dur = get_usecs() - g_start;
	dur_in_sec = (float)dur / 1000000;
	printf("g_nread(bytes read) = %lld\n", (long long)g_nread);
	printf("elapsed = %.2f sec ( %.0f usec )\n", dur_in_sec, dur);
	g_nread = g_nread / ( g_stride / CACHE_LINE_SIZE );
	bw = (float)g_nread / dur_in_sec / 1024 / 1024;
	printf("CPU%d: B/W = %.2f MB/s | ",cpuid, bw);
	printf("CPU%d: average = %.2f ns\n", cpuid, (dur*1000)/(g_nread/CACHE_LINE_SIZE));
	if (g_mon)
		mon_report();
	exit(0);
}

int64_t bench_read()
{
	int64_t i;
	int64_t sum = 0;
	for ( i = 0; i < g_mem_size/4; i+=(g_stride/4) ) {
		sum += g_mem_ptr[i];
	}
	g_nread += g_mem_size;
	return sum;
}

int bench_write()
{
	register int64_t i;
	for ( i = 0; i < g_mem_size/4; i+=(g_stride/4) ) {
		g_mem_ptr[i] = i;
	}
	g_nread += g_mem_size;
	return 1;
}

void usage(int argc, char *argv[])
{
	printf("Usage: $ %s [<option>]*\n\n", argv[0]);
	printf("-m <int>[M|G] : memory size in KB. default=%d KB. appending M or G will interpret the number as MB or GB respectively.\n", DEFAULT_ALLOC_SIZE_KB);
	printf("-a <read|write>	: access type - read, write. default=read\n");
	printf("-t <int> : time to run in sec. 0 means indefinite. default=5. \n");
	printf("-x : use hugepage.\n");
	printf("-r <int> : set real-time priority. default=0; 1(low)- 99(high) for SCHED_FIFO\n");
	printf("-c <int> : CPU to run.\n");
	printf("-i <int> : iterations. 0 means intefinite. default=0\n");
	printf("-p <int> : CFS priority (nice value). -20 (highest)..19 (lowest) \n");
	printf("-s <int> : stride in bytes (multiple of %d). default=%d\n", CACHE_LINE_SIZE, CACHE_LINE_SIZE);
	printf("-M : report L2 monitor counters for the run (use on one instance only).\n");
	printf("-R <list> : RCIDs to report, e.g. 0,1 (implies -M). default=all with activity\n");
	printf("-h : help\n");
	printf("\nExamples: \n$ bandwidth -m 8192 -a read -t 1 -c 2\n  <- 8MB read for 1 second on CPU 2\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int64_t sum = 0;
	unsigned finish = 5;
	int prio = 0;        
	int num_processors;
	int acc_type = READ;
	int opt;
	cpu_set_t cmask;
	int iterations = 0;
	int use_hugepage = 0;
	int i;
	int report = 0;
	struct sched_param param;

	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "s:m:a:t:c:i:p:r:xhMR:")) != -1) {
		switch (opt) {
		case 'm': /* set memory size */
			if (optarg[strlen(optarg)-1] == 'G' || optarg[strlen(optarg)-1] == 'g')
				g_mem_size = 1024 * 1024 * 1024 * strtol(optarg, NULL, 0);
			else if (optarg[strlen(optarg)-1] == 'M' || optarg[strlen(optarg)-1] == 'm')
				g_mem_size = 1024 * 1024 * strtol(optarg, NULL, 0);
			else
				g_mem_size = 1024 * strtol(optarg, NULL, 0);
			break;
		case 'a': /* set access type */
			if (!strcmp(optarg, "read"))
				acc_type = READ;
			else if (!strcmp(optarg, "write"))
				acc_type = WRITE;
			else
				exit(1);
			break;
			
		case 't': /* set time in secs to run */
			finish = strtol(optarg, NULL, 0);
			break;
		case 'x':
			use_hugepage = (use_hugepage) ? 0: 1;
			break;
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;
		case 'r':
			prio = strtol(optarg, NULL, 0);
			param.sched_priority = prio; /* 1(low)- 99(high) for SCHED_FIFO or SCHED_RR
						        0 for SCHED_OTHER or SCHED_BATCH */
			if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
				perror("sched_setscheduler failed");
			}
			break;
		case 'p': /* set priority */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		case 'i': /* iterations */
			iterations = strtol(optarg, NULL, 0);
			break;
		case 's':
			g_stride = strtol(optarg, NULL, 0);
			break;
		case 'M':
			report = 1;
			break;
		case 'R':
			report = 1;
			mon_parse_rcids(optarg);
			break;
		case 'h': 
			usage(argc, argv);
			break;
		}
	}

	/*
	 * allocate contiguous region of memory 
	 */ 
	if (use_hugepage) {
		unsigned long hugepage_size_kb = 0;

		g_mem_ptr = (int *)map_hugepage(g_mem_size, &hugepage_size_kb);
		if ((void *)g_mem_ptr == MAP_FAILED) {
			perror("mmap with hugepage failed");
			exit(1);
		}
		if (hugepage_size_kb > 0)
			printf("Using %luKB hugepage\n", hugepage_size_kb);
		else
			printf("Using default hugepage size\n");
	} else {
		g_mem_ptr = (int *)malloc(g_mem_size);
		if (g_mem_ptr == NULL) {
			perror("alloc failed");
			exit(1);
		}
		printf("Using malloc(), not very accurate\n");
	}

	memset((char *)g_mem_ptr, 1, g_mem_size);

	for (i = 0; i < g_mem_size / sizeof(int); i++)
		g_mem_ptr[i] = i;

	/* print experiment info before starting */
	printf("memsize=%ld KB, type=%s, cpuid=%d, stride=%d\n",
	       g_mem_size/1024,
	       ((acc_type==READ) ?"read": "write"),
		cpuid,
		g_stride);
	printf("stop at %d\n", finish);

	/* set signals to terminate once time has been reached */
	signal(SIGINT, &quit);
	if (finish > 0) {
		signal(SIGALRM, &quit);
		alarm(finish);
	}

	if (report)
		mon_map();

	/*
	 * actual memory access
	 */
	if (g_mon)
		mon_snapshot(&g_mon_start);
	g_start = get_usecs();
	for (i=0;; i++) {
		switch (acc_type) {
		case READ:
			sum += bench_read();
			break;
		case WRITE:
			sum += bench_write();
			break;
		}

		if (iterations > 0 && i+1 >= iterations)
			break;
	}
	printf("total sum = %ld\n", (long)sum);
	quit(0);
	return 0;
}
