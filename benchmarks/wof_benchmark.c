#define _GNU_SOURCE
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<math.h>
#include<time.h>
#include<sys/time.h>
#include <sched.h>
#include <pthread.h>
#include <errno.h>
#include <getopt.h>
#include <sys/syscall.h>

static int nr_threads;
static int ratio;
static long long int array_size;
static char* outfile;
pthread_mutex_t outfile_lock;

/*
 * gcc wof_load.c -lm -fopenmp -lpthread
 * Taskset high util thread to faster cpu and monitor impact on light util thread when setting it to faster or slower cpu will not impact workload much
 * taskset -c 12,6 ./a.out
 */

void tv_copy(struct timeval* tdest, struct timeval* tsrc){
	tdest->tv_sec = tsrc->tv_sec;
	tdest->tv_usec = tsrc->tv_usec;
}

void tvsub(struct timeval * tdiff, struct timeval * t1, struct timeval * t0)
{
	tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
	tdiff->tv_usec = t1->tv_usec - t0->tv_usec;
	if (tdiff->tv_usec < 0 && tdiff->tv_sec > 0) {
		tdiff->tv_sec--;
		tdiff->tv_usec += 1000000;
		if (tdiff->tv_usec < 0) {
			fprintf(stderr, "lat_fs: tvsub shows test time ran backwards!\n");
			exit(1);
		}
	}

	/* time shouldn't go backwards!!! */
	if (tdiff->tv_usec < 0 || t1->tv_sec < t0->tv_sec) {
		tdiff->tv_sec = 0;
		tdiff->tv_usec = 0;
	}
}

/*
 * returns the difference between start and stop in usecs.  Negative values
 * are turned into 0
 */
unsigned long long tvdelta(struct timeval *start, struct timeval *stop)
{
	struct timeval td;
	unsigned long long usecs;

	tvsub(&td, stop, start);
	usecs = td.tv_sec;
	usecs *= 1000000;
	usecs += td.tv_usec;
	return (usecs);
}

int stick_this_thread_to_core(int core_id) {
	int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (core_id < 0 || core_id >= num_cores)
		return EINVAL;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);

	pthread_t current_thread = pthread_self();    
	return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

struct data_passer {
	struct timeval timestamp;
	int cpu_in_use;
	unsigned long long delta;
};

typedef struct linked_list {
	struct data_passer data;
	struct linked_list* next;
}ll;

void insert_node(ll* tail, struct timeval *time1, unsigned long long delta, int cpu)
{
	ll* new_node = (ll*)malloc(sizeof(ll));
	tv_copy(&new_node->data.timestamp, time1);
	new_node->data.cpu_in_use = cpu;
	new_node->data.delta = delta;
	tail->next = new_node;
	new_node->next = NULL;
	return ;
}

void free_ll(ll*head){
	while(head){
		ll* temp = head->next;
		free(head);
		head = temp;
		if(temp)
		temp = temp->next;
	}
}


void* high_util(void* data)
{
	int a[array_size];
	int i = array_size/1000;
	int sum = 0;
	struct timeval t1,t2;
	struct data_passer* dp = (struct data_passer*)data;
	struct timeval first_wakeup;

	gettimeofday(&first_wakeup, NULL);

	printf("high util PID=%lu created tiemdiff=%llu from cpu=%d to=%d\n",
			syscall(SYS_gettid),tvdelta(&dp->timestamp,
				&first_wakeup),dp->cpu_in_use, sched_getcpu());

	for(int iter=0;iter<array_size;iter++)
		a[iter] = rand();

	//stick_this_thread_to_core(12);

	gettimeofday(&t1, NULL);
	while(i--)
	{
		for(int i=0;i<array_size;i++)sum += a[i];
	}
	gettimeofday(&t2, NULL);

	return NULL;
}

void check_cpu_changed(struct timeval *first_wakeup, ll* visited_cpu_tail)
{
	int cpu = sched_getcpu(); 
	int *current_cpu = &visited_cpu_tail->data.cpu_in_use;
	
	if(cpu != *current_cpu){
		struct timeval temp;
		gettimeofday(&temp, NULL);
		insert_node(visited_cpu_tail, &temp, tvdelta(first_wakeup, &temp), cpu);
		*current_cpu = cpu;
		tv_copy(first_wakeup, &temp);
	}
}

void* low_util(void *data)
{
	int i=array_size/1000;
	int a[array_size];
	int sum = 0;
	struct data_passer* dp = (struct data_passer*)data;
	struct timeval first_wakeup;
	int current_cpu = sched_getcpu();
	FILE* fd;
	ll* visited_cpus;
	ll* visited_cpus_head;

	gettimeofday(&first_wakeup, NULL);
	visited_cpus = (ll*)malloc(sizeof(ll));
	visited_cpus_head = visited_cpus;
	visited_cpus->data.cpu_in_use = current_cpu;
	visited_cpus->data.delta = tvdelta(&dp->timestamp, &first_wakeup);
	tv_copy(&visited_cpus->data.timestamp, &first_wakeup);


	printf("low util PID=%lu creation time=%llu from cpu=%d to=%d\n",
			syscall(SYS_gettid),tvdelta(&dp->timestamp,
			&first_wakeup),dp->cpu_in_use,current_cpu);
	
	for(int iter=0;iter<array_size;iter++)
		a[iter] = rand();

	while(i--)
	{
		for(int i=0;i<10000;i++)
		{
			for(int i=0;i<1000;i++)sum += a[i];
			check_cpu_changed(&first_wakeup, visited_cpus);
			if(visited_cpus->next)
			visited_cpus = visited_cpus->next;
			usleep(1);
		}
	}

	gettimeofday(&dp->timestamp, NULL);
	dp->cpu_in_use = sched_getcpu();
	high_util(data);
	
	visited_cpus = visited_cpus_head;
	pthread_mutex_lock(&outfile_lock);
	
	if(outfile[0]=='s')
		fd = stdout;
	else
		fd = fopen(outfile, "a");
	
	
	fprintf(fd, "%lu:Visited cpus and timedelta between them----------------------\n",syscall(SYS_gettid));
	while(visited_cpus){
		fprintf(fd, "PID=%lu cpus=%d timediff=%llu timestamp=%ld\n",syscall(SYS_gettid), visited_cpus->data.cpu_in_use,
				visited_cpus->data.delta, visited_cpus->data.timestamp.tv_sec*1000000+visited_cpus->data.timestamp.tv_usec);
		visited_cpus = visited_cpus->next;
	}
	fflush(fd);
	fclose(fd);
	pthread_mutex_unlock(&outfile_lock);

	return NULL;
}



enum {
	HELP_LONG_OPT = 1,
};
char *option_string = "t:r:n:o:";
static struct option long_options[] = {
	{"threads", required_argument, 0, 't'},
	{"ratio", required_argument, 0, 'r'},
	{"array-size", required_argument, 0, 'n'},
	{"output", required_argument, 0, 'o'},
	{"help", no_argument, 0, HELP_LONG_OPT},
	{0, 0, 0, 0}
};

static void print_usage(void)
{
	fprintf(stderr, "wofbench usage:\n"
			"\t-t (--nr-threads): number of threads (def: 1)\n"
			"\t-r (--ratio): ratio of threads in low utilization categor(def: 0)\n"
			"\t-n (--array-size): size of array (def: 10000)\n"
			"\t-o (--output): writes cpus covered per thread into a file(def: stdout)\n"
	       );
	exit(1);
}

static void parse_options(int ac, char **av)
{
	int c;

	while (1) {
		int option_index = 0;

		c = getopt_long(ac, av, option_string,
				long_options, &option_index);

		if (c == -1)
			break;

		switch(c) {
			case 't':
				nr_threads = atoi(optarg);
				break;
			case 'r':
				ratio = atoi(optarg);
				break;
			case 'n':
				sscanf(optarg, "%lld", &array_size);
				break;
			case 'o' :
				outfile = optarg;
				break;
			case '?':
			case HELP_LONG_OPT:
				print_usage();
				break;
			default:
				break;
		}
	}

	if (optind < ac) {
		fprintf(stderr, "Error Extra arguments '%s'\n", av[optind]);
		exit(1);
	}
}

int main(int argc, char**argv){
	struct data_passer dp;
	pthread_t *tid;
	nr_threads = 1;
	array_size = 10000;
	ratio = 0;
	outfile = "stdout";

	parse_options(argc, argv);
	printf("Running with array_size=%lld ratio=%d\n",array_size, ratio);

	srand(time(NULL));
	tid = (pthread_t*)malloc(sizeof(pthread_t)*nr_threads);

	system("/bin/bash ftrace_migration.sh 1 \"comm==\'wof_bench\'\"");
	for(int i=0; i<nr_threads; i++)
	{
		if(i >= nr_threads*ratio/100){
			gettimeofday(&dp.timestamp, NULL);
			dp.cpu_in_use = sched_getcpu();
			pthread_create(&tid[i], NULL, high_util, &dp);
		}
		else{
			gettimeofday(&dp.timestamp, NULL);
			dp.cpu_in_use = sched_getcpu();
			pthread_create(&tid[i],NULL, low_util, &dp);
		}
	}

	for(int i=0; i<nr_threads; i++)
		pthread_join(tid[i], NULL);
	//stop tracing
	system("/bin/bash ftrace_migration.sh 0");
	//move trace file here
	system("/bin/bash ftrace_migration.sh 2");

	return 0;
}
