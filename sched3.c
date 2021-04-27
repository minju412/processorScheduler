/**********************************************************************
 * Copyright (c) 2019-2021
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

/*====================================================================*/
/*          ******        DO NOT MODIFY THIS FILE        ******       */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

#include "types.h"
#include "list_head.h"

#include "parser.h"
#include "process.h"
#include "resource.h"

#include "sched.h"

//추가
#include <ctype.h>

/**
 * List head to hold the processes ready to run
 */
LIST_HEAD(readyqueue);

/**
 * The process that is currently running
 */
struct process *current = NULL;

/**
 * Number of generated ticks since the simulator was started
 */
unsigned int ticks = 0;

/**
 * Resources in the system.
 */
struct resource resources[NR_RESOURCES];

/**
 * Following code is to maintain the simulator itself.
 */
struct resource_schedule {
	int resource_id;
	int at;
	int duration;
	struct list_head list;
};

static LIST_HEAD(__forkqueue);

bool quiet = false;

static const char * __process_status_sz[] = {
	"RDY",
	"RUN",
	"WAT",
	"EXT",
};

/**
 * Assorted schedulers
 */
extern struct scheduler fifo_scheduler;
extern struct scheduler sjf_scheduler;
extern struct scheduler srtf_scheduler;
extern struct scheduler rr_scheduler;
extern struct scheduler prio_scheduler;
extern struct scheduler pa_scheduler;
extern struct scheduler pcp_scheduler;
extern struct scheduler pip_scheduler;

static struct scheduler *sched = &fifo_scheduler;

void dump_status(void)
{
	struct process *p;

	printf("***** CURRENT *********\n");
	if (current) {
		printf("%2d (%s): %d + %d/%d at %d\n",
				current->pid, __process_status_sz[current->status],
				current->__starts_at,
				current->age, current->lifespan, current->prio);
	}

	printf("***** READY QUEUE *****\n");
	list_for_each_entry(p, &readyqueue, list) {
		printf("%2d (%s): %d + %d/%d at %d\n",
				p->pid, __process_status_sz[p->status],
				p->__starts_at, p->age, p->lifespan, p->prio);
	}

	printf("***** RESOURCES *******\n");
	for (int i = 0; i < NR_RESOURCES; i++) {
		struct resource *r = resources + i;;
		if (r->owner || !list_empty(&r->waitqueue)) {
			printf("%2d: owned by ", i);
			if (r->owner) {
				printf("%d\n", r->owner->pid);
			} else {
				printf("no one\n");
			}

			list_for_each_entry(p, &r->waitqueue, list) {
				printf("    %d is waiting\n", p->pid);
			}
		}
	}
	printf("\n\n");

	return;
}

#define __print_event(pid, string, args...) do { \
	fprintf(stderr, "%3d: ", ticks); \
	for (int i = 0; i < pid; i++) { \
		fprintf(stderr, "    "); \
	} \
	fprintf(stderr, string "\n", ##args); \
} while (0);

static inline bool strmatch(char * const str, const char *expect)
{
	return (strlen(str) == strlen(expect)) && (strncmp(str, expect, strlen(expect)) == 0);
}

static void __briefing_process(struct process *p)
{
	struct resource_schedule *rs;

	if (quiet) return;

	printf("- Process %d: Forked at tick %d and run for %d tick%s with initial priority %d\n",
				p->pid, p->__starts_at, p->lifespan,
				p->lifespan >= 2 ? "s" : "", p->prio);

	list_for_each_entry(rs, &p->__resources_to_acquire, list) {
		printf("    Acquire resource %d at %d for %d\n", rs->resource_id, rs->at, rs->duration);
	}
}

static int __load_script(char * const filename)
{
	char line[256];
	struct process *p = NULL;

	FILE *file = fopen(filename, "r");
	while (fgets(line, sizeof(line), file)) {
		char *tokens[32] = { NULL };
		int nr_tokens;

		parse_command(line, &nr_tokens, tokens);

		if (nr_tokens == 0) continue;

		if (strmatch(tokens[0], "process")) {
			assert(nr_tokens == 2);
			/* Start processor description */
			p = malloc(sizeof(*p));
			memset(p, 0x00, sizeof(*p));

			p->pid = atoi(tokens[1]);

			INIT_LIST_HEAD(&p->list);
			INIT_LIST_HEAD(&p->__resources_to_acquire);
			INIT_LIST_HEAD(&p->__resources_holding);

			continue;
		} else if (strmatch(tokens[0], "end")) {
			/* End of process description */
			struct resource_schedule *rs;
			assert(p);

			list_add_tail(&p->list, &__forkqueue);

			__briefing_process(p);
			p = NULL;

			continue;
		}

		if (strmatch(tokens[0], "lifespan")) {
			assert(nr_tokens == 2);
			p->lifespan = atoi(tokens[1]);
		} else if (strmatch(tokens[0], "prio")) {
			assert(nr_tokens == 2);
			p->prio = p->prio_orig = atoi(tokens[1]);
		} else if (strmatch(tokens[0], "start")) {
			assert(nr_tokens == 2);
			p->__starts_at = atoi(tokens[1]);
		} else if (strmatch(tokens[0], "acquire")) {
			struct resource_schedule *rs;
			assert(nr_tokens == 4);

			rs = malloc(sizeof(*rs));

			rs->resource_id = atoi(tokens[1]);
			rs->at = atoi(tokens[2]);
			rs->duration = atoi(tokens[3]);

			list_add_tail(&rs->list, &p->__resources_to_acquire);
		} else {
			fprintf(stderr, "Unknown property %s\n", tokens[0]);
			return false;
		}
	}
	fclose(file);
	if (!quiet) printf("\n");
	return true;
}


/**
 * Fork process on schedule
 */
static int __fork_on_schedule()
{
	int nr_forked = 0;
	struct process *p, *tmp;
	list_for_each_entry_safe(p, tmp, &__forkqueue, list) {
		if (p->__starts_at <= ticks) {
			list_move_tail(&p->list, &readyqueue);
			p->status = PROCESS_READY;
			__print_event(p->pid, "N");
			if (sched->forked) sched->forked(p);
			nr_forked++;
		}
	}
	return nr_forked;
}

/**
 * Exit the process
 */
static void __exit_process(struct process *p)
{
	/* Make sure the process is not attached to some list head */
	assert(list_empty(&p->list));

	/* Make sure the process is not holding any resource */
	assert(list_empty(&p->__resources_holding));

	/* Make sure there is no pending resource to acquire */
	assert(list_empty(&p->__resources_to_acquire));

	if (sched->exiting) sched->exiting(p);

	__print_event(p->pid, "X");

	free(p);
}


/**
 * Process resource acqutision
 */
static bool __run_current_acquire()
{
	struct resource_schedule *rs, *tmp;

	list_for_each_entry_safe(rs, tmp, &current->__resources_to_acquire, list) {
		if (rs->at == current->age) {
			assert(sched->acquire && "scheduler.acquire() not implemented");

			/* Callback to acquire the resource */
			if (sched->acquire(rs->resource_id)) {
				list_move_tail(&rs->list, &current->__resources_holding);

				__print_event(current->pid, "+%d", rs->resource_id);
			} else {
				return false;
			}
		}
	}

	return true;
}

/**
 * Process resource release
 */
static void __run_current_release()
{
	struct resource_schedule *rs, *tmp;

	list_for_each_entry_safe(rs, tmp, &current->__resources_holding, list) {
		if (--rs->duration == 0) {
			assert(sched->release && "scheduler.release() not implemented");

			/* Callback the release() */
			sched->release(rs->resource_id);

			__print_event(current->pid, "-%d", rs->resource_id);

			list_del(&rs->list);
			free(rs);
		}
	}
}

////////임의 추가////////////////////////////////

int parse_command(char *command, int *nr_tokens, char *tokens[])
{
	char *curr = command;
	int token_started = false;
	*nr_tokens = 0;

	while (*curr != '\0') {
		if (isspace(*curr)) {
			*curr = '\0';
			token_started = false;
		} else {
			if (!token_started) {
				tokens[*nr_tokens] = curr;
				*nr_tokens += 1;
				token_started = true;
			}
		}

		curr++;
	}

	/* Remove comments */
	for (int i = 0; i < *nr_tokens; i++) {
		if (strncmp(tokens[i], "#", strlen("#")) == 0) {
			*nr_tokens = i;
			tokens[i] = NULL;
			break;
		}
	}

	return (*nr_tokens > 0);
}


bool fcfs_acquire(int resource_id)
{
	struct resource *r = resources + resource_id;

	if (!r->owner) {
		/* This resource is not owned by any one. Take it! */
		r->owner = current;
		return true;
	}

	/* OK, this resource is taken by @r->owner. */
	
	/* Update the current process state */
	current->status = PROCESS_WAIT;

	/* And append current to waitqueue */
	list_add_tail(&current->list, &r->waitqueue);

	/**
	 * And return false to indicate the resource is not available.
	 * The scheduler framework will soon call schedule() function to
	 * schedule out current and to pick the next process to run.
	 */
	return false;
}


void fcfs_release(int resource_id)
{
	struct resource *r = resources + resource_id;

	/* Ensure that the owner process is releasing the resource */
	assert(r->owner == current);

	/* Un-own this resource */
	r->owner = NULL;

	/* Let's wake up ONE waiter (if exists) that came first */
	if (!list_empty(&r->waitqueue)) {
		struct process *waiter =
				list_first_entry(&r->waitqueue, struct process, list);

		/**
		 * Ensure the waiter is in the wait status
		 */
		assert(waiter->status == PROCESS_WAIT);

		/**
		 * Take out the waiter from the waiting queue. Note we use
		 * list_del_init() over list_del() to maintain the list head tidy
		 * (otherwise, the framework will complain on the list head
		 * when the process exits).
		 */
		list_del_init(&waiter->list);

		/* Update the process status */
		waiter->status = PROCESS_READY;

		/**
		 * Put the waiter process into ready queue. The framework will
		 * do the rest.
		 */
		list_add_tail(&waiter->list, &readyqueue);
	}
}

static int fifo_initialize(void)
{
	return 0;
}

static void fifo_finalize(void)
{
}

static struct process *fifo_schedule(void)
{
	struct process *next = NULL;

	/* You may inspect the situation by calling dump_status() at any time */
	// dump_status();

	/**
	 * When there was no process to run in the previous tick (so does
	 * in the very beginning of the simulation), there will be
	 * no @current process. In this case, pick the next without examining
	 * the current process. Also, when the current process is blocked
	 * while acquiring a resource, @current is (supposed to be) attached
	 * to the waitqueue of the corresponding resource. In this case just
	 * pick the next as well.
	 */
	if (!current || current->status == PROCESS_WAIT) {
		goto pick_next;
	}

	/* The current process has remaining lifetime. Schedule it again */
	if (current->age < current->lifespan) {
		return current;
	}

pick_next:
	/* Let's pick a new process to run next */

	if (!list_empty(&readyqueue)) {
		/**
		 * If the ready queue is not empty, pick the first process
		 * in the ready queue
		 */
		next = list_first_entry(&readyqueue, struct process, list);

		/**
		 * Detach the process from the ready queue. Note we use list_del_init()
		 * instead of list_del() to maintain the list head tidy. Otherwise,
		 * the framework will complain (assert) on process exit.
		 */
		list_del_init(&next->list);
	}

	/* Return the next process to run */
	return next;
}

struct scheduler fifo_scheduler = {
	.name = "FIFO",
	.acquire = fcfs_acquire,
	.release = fcfs_release,
	.initialize = fifo_initialize,
	.finalize = fifo_finalize,
	.schedule = fifo_schedule,
};


/***********************************************************************
 * SJF scheduler
 ***********************************************************************/
static struct process *sjf_schedule(void)
{
    struct list_head* ptr;
    struct process* prc = NULL;

    int min=1000; //lifespan의 min 찾기 위해
    int cnt=0;
	
    if (!current || current->status == PROCESS_WAIT) {
		goto pick_next;
	}
    /* The current process has remaining lifetime. Schedule it again */
	if (current->age < current->lifespan) {
		return current;
	}

pick_next:

    list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            cnt++;
        }

    if(cnt!=0){
        list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (min > prc->lifespan)
                min = prc->lifespan;
        }

        list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (min == prc->lifespan){
                list_del_init(&prc->list);
                return prc;
            }
        }

    }

    return prc;
}

struct scheduler sjf_scheduler = {
	.name = "Shortest-Job First",
	.acquire = fcfs_acquire, /* Use the default FCFS acquire() */
	.release = fcfs_release, /* Use the default FCFS release() */
	.schedule = sjf_schedule,		 /* TODO: Assign sjf_schedule()
								to this function pointer to activate
								SJF in the system */
};

/***********************************************************************
 * SRTF scheduler
 ***********************************************************************/
static struct process *srtf_schedule(void)
{
    // if(sched->forked){ //새로운 프로세스 생성됐다면..?
    //     sched->forked(p);
    // }

    struct list_head* ptr;
    struct process* prc = NULL;

	int min=1000; //lifespan의 min 찾기 위해
    int cnt=0;
	int cur_remain=0;

	list_for_each(ptr, &readyqueue){
        prc = list_entry(ptr, struct process, list);
        cnt++;
    }

    if(cnt!=0){
        list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (min > prc->lifespan)
            	min = prc->lifespan;
        }
	}

    if (!current || current->status == PROCESS_WAIT) { //이때는 sjf랑 똑같이?
		goto pick_next;
	}
    /* The current process has remaining lifetime. Schedule it again */
	if (current->age < current->lifespan) { //이 시점에 다른 프로세스의 lifespan들과 비교 -> preemptive 가능
		
		cur_remain = current->lifespan - current->age;
	
		if(min < cur_remain){
				list_for_each(ptr, &readyqueue){
            		prc = list_entry(ptr, struct process, list);
            		if (min == prc->lifespan){
                		list_del_init(&prc->list);
						list_add(&current->list, &readyqueue);
                		return prc;
            		}
        		}
			}
			else{
				return current;
			}

	}

pick_next:
	
	list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            cnt++;
        }

    if(cnt!=0){
        list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (min > prc->lifespan)
            	min = prc->lifespan;
        }

        	list_for_each(ptr, &readyqueue){
            	prc = list_entry(ptr, struct process, list);
            	if (min == prc->lifespan){
                	list_del_init(&prc->list);
                	return prc;
            	}
        	}
		
    }

	return prc;

}

struct scheduler srtf_scheduler = {
	.name = "Shortest Remaining Time First",
	.acquire = fcfs_acquire, /* Use the default FCFS acquire() */
	.release = fcfs_release, /* Use the default FCFS release() */
	/* You need to check the newly created processes to implement SRTF.
	 * Use @forked() callback to mark newly created processes */
	/* Obviously, you should implement srtf_schedule() and attach it here */
    .schedule = srtf_schedule,
};

/***********************************************************************
 * Round-robin scheduler
 ***********************************************************************/

int ticks_cnt=-1; 

static struct process *rr_schedule(void)
{
    struct process* prc = NULL;
	ticks_cnt++;

	if (!current || current->status == PROCESS_WAIT) { 
		goto pick_next;
	}
    /* The current process has remaining lifetime. Schedule it again */
	if (current->age < current->lifespan) { //current가 돌고 있어도 타임퀀텀(=1) 지나면 preemption
	
		if(ticks == ticks_cnt){ //preemption
			if (!list_empty(&readyqueue)) {
				prc = list_first_entry(&readyqueue, struct process, list);
				list_del_init(&prc->list);

				//if(current->lifespan!=0)
					list_add_tail(&current->list, &readyqueue);
				return prc;
			}		
		}		
		return current;
	}

pick_next:
	if (!list_empty(&readyqueue)) {
		prc = list_first_entry(&readyqueue, struct process, list);
		list_del_init(&prc->list);
	}
	return prc;
}

struct scheduler rr_scheduler = {
	.name = "Round-Robin",
	.acquire = fcfs_acquire, /* Use the default FCFS acquire() */
	.release = fcfs_release, /* Use the default FCFS release() */
	/* Obviously, you should implement rr_schedule() and attach it here */
	.schedule = rr_schedule,
};

/***********************************************************************
 * Priority scheduler
 ***********************************************************************/

bool prio_acquire(int resource_id){
	//true on successful acquision
	//false if the resource is already held by others or unavailable

	struct resource *r = resources + resource_id;



	//dump_status();
	if (!r->owner) {
		r->owner = current;
		
		// current->status = PROCESS_RUNNING;
    	// list_del_init(&current->list);

		current->status = PROCESS_WAIT;
    	//list_add_tail(&current->list, &r->waitqueue);
		list_add_tail(&current->list, &readyqueue);


		return true;
	}
	
	
	current->status = PROCESS_WAIT;
    list_add_tail(&current->list, &r->waitqueue);
	//dump_status();
		
		// if(!list_empty(&r->waitqueue)){
		// 	printf("current:%d owner:%d\n", current->pid, r->owner->pid);
			
		// 	current->status = PROCESS_WAIT;
   		// 	list_add_tail(&current->list, &r->waitqueue);
		// 	r->owner->status = PROCESS_RUNNING; //?
		// 	//list_del_init(&r->owner->list); //?

		// // //if(r->owner->status == PROCESS_WAIT){
		// // 	printf("okkkkk\n");
		// // 	current = list_first_entry(&r->waitqueue, struct process, list);
		// // 	printf("current=%d\n", current->pid);
		// // 	//current=r->owner;
		// // 	r->owner->status = PROCESS_RUNNING; //?
		// // 	list_del_init(&r->owner->list); //?
		// }

	return false;
}

void prio_release(int resource_id){

	//추가
	int max=0;
	int cnt=0; //확인용
	struct list_head* ptr;
	struct process* prc = NULL;

	struct process* waiter = NULL;
	struct resource *r = resources + resource_id;

	assert(r->owner == current);
	r->owner = NULL;

	list_for_each(ptr, &r->waitqueue){
        prc = list_entry(ptr, struct process, list);
		cnt++;
    }
    //printf("max=%d\n", max); //확인용
	//printf("cnt=%d\n", cnt); //확인용

	//놓을 때는 어떻게 처리할건지?
	if (!list_empty(&r->waitqueue)) {
		// struct process *waiter =
		// 	list_first_entry(&r->waitqueue, struct process, list);
		//printf("max=%d\n", max); //확인용
		list_for_each(ptr, &r->waitqueue){
        	prc = list_entry(ptr, struct process, list);
			if(max < prc->prio)
				max = prc->prio;
    	}
		//printf("max=%d\n", max); //확인용

		//wake up high priority waiter! 
		list_for_each(ptr, &r->waitqueue){
            prc = list_entry(ptr, struct process, list);
			if(prc->prio==max){
				waiter = prc;
				goto next;
			}
        }

		next:
		assert(waiter->status == PROCESS_WAIT);
		list_del_init(&waiter->list);
		waiter->status = PROCESS_READY;
		list_add_tail(&waiter->list, &readyqueue);
	}

}

 static struct process *prio_schedule(void)
 {
	 //숫자가 클수록 priority 높은 것
	struct list_head* ptr;
    struct process* prc = NULL;

    int max=0; //prio의 max 찾기 위해
    int cnt=0;
	
    if (!current || current->status == PROCESS_WAIT) {
		goto pick_next;
	}
    /* The current process has remaining lifetime. Schedule it again */
	if (current->age < current->lifespan) {
		return current;
	}

pick_next:

    list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            cnt++;
        }

    if(cnt!=0){

		 list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (max < prc->prio)
                max = prc->prio;
        }

        list_for_each(ptr, &readyqueue){
            prc = list_entry(ptr, struct process, list);
            if (max == prc->prio){
                list_del_init(&prc->list);
                return prc;
            }
        }

	}

	return prc;

 }

struct scheduler prio_scheduler = {
	.name = "Priority",
	/**
	 * Implement your own acqure/release function to make priority
	 * scheduler correct.
	 */
	.acquire = prio_acquire,
	.release = prio_release,
	/* Implement your own prio_schedule() and attach it here */
	.schedule = prio_schedule,
};


/***********************************************************************
 * Priority scheduler with aging
 ***********************************************************************/
// bool pa_acquire(int resource_id){
// }

// void pa_release(int resource_id){
// }

// struct scheduler pa_scheduler = {
// 	.name = "Priority + aging",
// 	/**
// 	 * Implement your own acqure/release function to make priority
// 	 * scheduler correct.
// 	 */
// 	/* Implement your own prio_schedule() and attach it here */
// };





/***********************************************************************
 * The main loop for the scheduler simulation
 */
static void __do_simulation(void)
{
	assert(sched->schedule && "scheduler.schedule() not implemented");

	while (true) {
		struct process *prev;

		/* Fork processes on schedule */
		__fork_on_schedule();

		/* Ask scheduler to pick the next process to run */
		prev = current;
		current = sched->schedule();

		/* If the system ran a process in the previous tick, */
		if (prev) {
			/* Update the process status */
			if (prev->status == PROCESS_RUNNING) {
				prev->status = PROCESS_READY;
			}

			/* Decommission it if completed */
			if (prev->age == prev->lifespan) {
				prev->status = PROCESS_EXIT;
				__exit_process(prev);
			}
		}

		/* No process is ready to run at this moment */
		if (!current) {
			/* Quit simulation if no pending process exists */
			if (list_empty(&readyqueue) && list_empty(&__forkqueue)) {
				break;
			}

			/* Idle temporarily */
			fprintf(stderr, "%3d: idle\n", ticks);
		} else {

			/* Execute the current process */
			current->status = PROCESS_RUNNING;

			/* Ensure that @current is detached from any list */
			assert(list_empty(&current->list));

			/* Try acquiring scheduled resources */
			if (__run_current_acquire()) {
				/* Succesfully acquired all the resources to make a progress! */
				__print_event(current->pid, "%d", current->pid);

				/* So, it ages by one tick */
				current->age++;

				/* And performs scheduled releases */
				__run_current_release();
			} else {
				/**
				 * The current is blocked while acquiring resource(s).
				 * In this case, @current could not make a progress in this tick
				 */
				__print_event(current->pid, "=");

				/* Thus, it is not get aged nor unable to perform releases */
			}
		}

		/* Increase the tick counter */
		ticks++;
	}
}


static void __initialize(void)
{
	INIT_LIST_HEAD(&readyqueue);

	for (int i = 0; i < NR_RESOURCES; i++) {
		resources[i].owner = NULL;
		INIT_LIST_HEAD(&(resources[i].waitqueue));
	}

	INIT_LIST_HEAD(&__forkqueue);

	if (quiet) return;
	printf("               _              _ \n");
	printf("              | |            | |\n");
	printf("      ___  ___| |__   ___  __| |\n");
	printf("     / __|/ __| '_ \\ / _ \\/ _` |\n");
	printf("     \\__ \\ (__| | | |  __/ (_| |\n");
	printf("     |___/\\___|_| |_|\\___|\\__,_|\n");
	printf("\n");
	printf("                                 2021 Spring\n");
	printf("      Simulating %s scheduler\n", sched->name);
	printf("\n");
	printf("****************************************************\n");
	printf("   N: Forked\n");
	printf("   X: Finished\n");
	printf("   =: Blocked\n");
	printf("  +n: Acquire resource n\n");
	printf("  -n: Release resource n\n");
	printf("\n");
}


static void __print_usage(char * const name)
{
	printf("Usage: %s {-q} -[f|s|S|r|a|p|i] [process script file]\n", name);
	printf("\n");
	printf("  -q: Run quietly\n\n");
	printf("  -f: Use FIFO scheduler (default)\n");
	printf("  -s: Use SJF scheduler\n");
	printf("  -S: Use SRTF scheduler\n");
	printf("  -r: Use Round-robin scheduler\n");
	printf("  -p: Use Priority scheduler\n");
	printf("  -a: Use Priority scheduler with aging\n");
	printf("  -c: Use Priority scheduler with PCP\n");
	printf("  -i: Use Priority scheduler with PIP\n");
	printf("\n");
}


int main(int argc, char * const argv[])
{
	int opt;
	char *scriptfile;

	while ((opt = getopt(argc, argv, "qfsSrpaich")) != -1) {
		switch (opt) {
		case 'q':
			quiet = true;
			break;

		case 'f':
			sched = &fifo_scheduler;
			break;
		case 's':
			sched = &sjf_scheduler;
			break;
		case 'S':
			sched = &srtf_scheduler;
			break;
		case 'r':
			sched = &rr_scheduler;
			break;
		case 'p':
			sched = &prio_scheduler;
			break;
		case 'a':
			//sched = &pa_scheduler;
			break;
		case 'i':
			//sched = &pip_scheduler;
			break;
		case 'c':
			//sched = &pcp_scheduler;
			break;
		case 'h':
		default:
			__print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		__print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	scriptfile = argv[optind];

	__initialize();

	if (!__load_script(scriptfile)) {
		return EXIT_FAILURE;
	}

	if (sched->initialize && sched->initialize()) {
		return EXIT_FAILURE;
	}

	__do_simulation();

	if (sched->finalize) {
		sched->finalize();
	}

	return EXIT_SUCCESS;
}
/*          ******        DO NOT MODIFY THIS FILE        ******       */
/*====================================================================*/
