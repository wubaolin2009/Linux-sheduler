/* this is the original algorithm of linux 2.6.21 or before.
  * In order to rejudge its performance under Embede System
  * I migrated it to the newest kernel 
  * Hope it will work 
  * We name it dq because of its double queue design
  * No Smp or group is supported yet
  * Wubaolin Nov 18th, 2010                                                      
  */

  /*
 * All the scheduling class methods:
 */
#include <linux/latencytop.h>
#include <linux/sched.h>
#define INTERACTIVE_DELTA 2
#define PRIO_BONUS_RATIO	 25
#define MAX_BONUS		(MAX_USER_PRIO * PRIO_BONUS_RATIO / 100)
#define SCALE(v1,v1_max,v2_max) \
	(v1) * (v2_max) / (v1_max)
#define DELTA(p) \
	(SCALE(TASK_NICE(p) + 20, 40, MAX_BONUS) - 20 * MAX_BONUS / 40 + \
		INTERACTIVE_DELTA)
/*
 * Some helpers for converting nanosecond timing to jiffy resolution
 */
//#define NS_TO_JIFFIES(TIME)	((TIME) / (1000000000 / HZ))
#define JIFFIES_TO_NS(TIME)	((TIME) * (1000000000 / HZ))
#define TASK_INTERACTIVE(p) \
	((p)->prio <= (p)->static_prio - DELTA(p))

#define EXIT_WEIGHT		  3
#define CHILD_PENALTY		 95
#define PARENT_PENALTY		100

#define ON_RUNQUEUE_WEIGHT	 30
#define MAX_SLEEP_AVG		(DEF_TIMESLICE * MAX_BONUS)

#define NS_MAX_SLEEP_AVG	(JIFFIES_TO_NS(MAX_SLEEP_AVG))

#define CURRENT_BONUS(p) \
	(NS_TO_JIFFIES((p)->sleep_avg) * MAX_BONUS / \
		MAX_SLEEP_AVG)

#define STARVATION_LIMIT	(MAX_SLEEP_AVG)

#define GRANULARITY	(10 * HZ / 1000 ? : 1)


#define batch_task(p)		(unlikely((p)->policy == SCHED_BATCH))



#define INTERACTIVE_SLEEP(p) \
	(JIFFIES_TO_NS(MAX_SLEEP_AVG * \
		(MAX_BONUS / 2 + DELTA((p)) + 1) / MAX_BONUS - 1))

#define TIMESLICE_GRANULARITY(p)	(GRANULARITY * \
		(1 << (((MAX_BONUS - CURRENT_BONUS(p)) ? : 1) - 1)))
#define TASK_NONINTERACTIVE	64
static unsigned long now;
extern  int effective_prio(struct task_struct *p);

static const struct sched_class dq_sched_class;


/*
 * Recalculate p->normal_prio and p->prio after having slept,
 * updating the sleep-average too:
 */
static int recalc_task_prio(struct task_struct *p, unsigned long long now)
{
	/* Caller must always ensure 'now >= p->timestamp' */
	unsigned long sleep_time = now - p->timestamp;

	if (batch_task(p))
		sleep_time = 0;

	if (likely(sleep_time > 0)) {
		/*
		 * This ceiling is set to the lowest priority that would allow
		 * a task to be reinserted into the active array on timeslice
		 * completion.
		 */
		unsigned long ceiling = INTERACTIVE_SLEEP(p);

		if (p->mm && sleep_time > ceiling && p->sleep_avg < ceiling) {
			/*
			 * Prevents user tasks from achieving best priority
			 * with one single large enough sleep.
			 */
			p->sleep_avg = ceiling;
			/*
			 * Using INTERACTIVE_SLEEP() as a ceiling places a
			 * nice(0) task 1ms sleep away from promotion, and
			 * gives it 700ms to round-robin with no chance of
			 * being demoted.  This is more than generous, so
			 * mark this sleep as non-interactive to prevent the
			 * on-runqueue bonus logic from intervening should
			 * this task not receive cpu immediately.
			 */
			p->sleep_type = SLEEP_NONINTERACTIVE;
		} else {
			/*
			 * Tasks waking from uninterruptible sleep are
			 * limited in their sleep_avg rise as they
			 * are likely to be waiting on I/O
			 */
			if (p->sleep_type == SLEEP_NONINTERACTIVE && p->mm) {
				if (p->sleep_avg >= ceiling)
					sleep_time = 0;
				else if (p->sleep_avg + sleep_time >=
					 ceiling) {
						p->sleep_avg = ceiling;
						sleep_time = 0;
				}
			}

			/*
			 * This code gives a bonus to interactive tasks.
			 *
			 * The boost works by updating the 'average sleep time'
			 * value here, based on ->timestamp. The more time a
			 * task spends sleeping, the higher the average gets -
			 * and the higher the priority boost gets as well.
			 */
			p->sleep_avg += sleep_time;

		}
		if (p->sleep_avg > NS_MAX_SLEEP_AVG)
			p->sleep_avg = NS_MAX_SLEEP_AVG;
	}

	return effective_prio(p);
}

static void __enqueue_task_dq_common(struct rq* rq,struct task_struct* p,int flags)
{
//printk("in enqueue_task_dq new pid is %d\n",p->pid);
	int old_state = p->state;
	if (old_state == TASK_UNINTERRUPTIBLE) {
		rq->nr_uninterruptible--;
		/*
		 * Tasks on involuntary sleep don't earn
		 * sleep_avg beyond just interactive state.
		 */
		p->sleep_type = SLEEP_NONINTERACTIVE;
	} else

	/*
	 * Tasks that have marked their sleep as noninteractive get
	 * woken up with their sleep average not weighted in an
	 * interactive way.
	 */
		if (old_state & TASK_NONINTERACTIVE)
			p->sleep_type = SLEEP_NONINTERACTIVE;


	/* From wake_up_new
	
	/* Do sth from the activate functions in the 2.6.21 Kernel
	  */
	now = sched_clock();
	//printk("after shced_clock\n");
	p->prio = recalc_task_prio(p, now);
	//printk("prio is %d\n",p->prio);

	/*
	 * This checks to make sure it's not an uninterruptible task
	 * that is now waking up.
	 */
	if (p->sleep_type == SLEEP_NORMAL) {
		/*
		 * Tasks which were woken up by interrupts (ie. hw events)
		 * are most likely of interactive nature. So we give them
		 * the credit of extending their sleep time to the period
		 * of time they spend on the runqueue, waiting for execution
		 * on a CPU, first time around:
		 */
		if (in_interrupt())
			p->sleep_type = SLEEP_INTERRUPTED;
		else {
			/*
			 * Normal first-time wakeups get a credit too for
			 * on-runqueue time, but it will be weighted down:
			 */
			p->sleep_type = SLEEP_INTERACTIVE;
		}
	}
	p->timestamp = now;
	//sched_info_queued(p);
}

static void __enqueue_task_dq_target(struct rq* rq,struct prio_array* to_array,struct task_struct* p,int flags)
{
	
        if(p->prio < 100 )
	{
		printk("BUG::prio bug here pid is %d,prio is %d\n",p->pid,p->prio);
	} 

	list_add_tail(&p->run_list, to_array->queue + p->prio);

	__set_bit(p->prio, to_array->bitmap);
	to_array->nr_active++;
	p->array = to_array;

}

static void
__enqueue_task_dq_active(struct rq* rq,struct task_struct* p,int flags)
{
	//__enqueue_task_dq_common(rq,p,flags);
	__enqueue_task_dq_target(rq,rq->active,p,flags);

}

static void 
__enqueue_task_dq_expired(struct rq* rq,struct task_struct* p,int flags)
{
	__enqueue_task_dq_target(rq,rq->expired,p,flags);
}



 static void
enqueue_task_dq(struct rq *rq, struct task_struct *p,int flags)
{

//	printk("in enqueue_task_dq\n");
//	printk("in enqueue_task_dq pid is %d\n",p->pid);
	if (batch_task(p))
		__enqueue_task_dq_expired(rq,p,0);
	else
		__enqueue_task_dq_active(rq,p,0);
		
	//printk("nr_running is %d\n",rq->nr_running);
	//printk("after sched_info_queued\n");
	//printk("runlist is %x,to_array is %x\n",p->run_list,to_array);
	//struct list_head* p1 = &p->run_list;
	//struct list_head* p2 = &(to_array->queue);
	//struct list_head* p3 = p2 + p->prio;
	//printk("p1 is %x %x %x\n",p1,p2,p3);
	
	
	//printk("pid is %d\n",p->pid);
	//printk("ret from enqueue\n");
	//if(p->pid == 2)
		//dump_stack();
}

 static void __dequeue_task_dq_from_source(struct prio_array* source, struct task_struct *p, int flags)
 {
 	struct prio_array* from_array = source;
	 
	from_array->nr_active--;
	list_del(&p->run_list);
	int i = 0;
	struct task_struct* next;
	if (list_empty(from_array->queue + p->prio))
	{
	__clear_bit(p->prio, from_array->bitmap);

	}		
	p->array = NULL;
 }
 	

static void __dequeue_task_dq_from_active(struct rq *rq, struct task_struct *p, int flags)
{
	__dequeue_task_dq_from_source(rq->active,p,flags);


}

static void __dequeue_task_dq_from_expired(struct rq *rq, struct task_struct *p, int flags)
{
	__dequeue_task_dq_from_source(rq->expired,p,flags);

}


static void dequeue_task_dq(struct rq *rq, struct task_struct *p, int sleep)
{
//	printk("we are in dequeue_task_dq\n");

//	printk("we are in dequeue_task_dq pid is %d\n",p->pid);
	struct prio_array* source = p->array;
	if(source== NULL)
	{
		source = rq->active;
		printk("we in dequeue_task_dq, but p->array is NULL, pid is %d\n",p->pid);
			printk("yes we are DEQUEUE_SLEEP\n");
			//p->array = rq->active;
			return;
	}
		

	__dequeue_task_dq_from_source(source,p,sleep);
}

/*
 * Put task to the end of the run list without the overhead of dequeue
 * followed by enqueue.
 */
static void requeue_task(struct task_struct *p, struct prio_array *array)
{
	printk("requeue_task,pid is %d\n",p->pid);
	list_move_tail(&p->run_list, array->queue + p->prio);
}

static inline void
enqueue_task_head(struct task_struct *p, struct prio_array *array)
{
	list_add(&p->run_list, array->queue + p->prio);
	__set_bit(p->prio, array->bitmap);
	array->nr_active++;
	p->array = array;
}

static void yield_task_dq(struct rq *rq)
{
	/* I don't know the meaning of this function 
	  * Leave it blank now 
	  */
	
}

/*
 * Preempt the current task with a newly woken task if needed:
 * Many feature can't be modified without deep understanding of 
 * The scheduler 's scheme, Leave it now
 */
static void check_preempt_wakeup_dq(struct rq *rq, struct task_struct *p, int wake_flags)
{
	 
	struct task_struct *curr = rq->curr;

	if (unlikely(p->sched_class != &dq_sched_class))
		return;

	if (test_tsk_need_resched(curr))
		return;

	if (p->prio < rq->curr->prio) {
		resched_task(rq->curr);
		return;
	}


}

static inline int interactive_sleep(enum sleep_type sleep_type)
{
	return (sleep_type == SLEEP_INTERACTIVE ||
		sleep_type == SLEEP_INTERRUPTED);
}



static struct task_struct* __prev = NULL;
static struct task_struct *pick_next_task_dq(struct rq *rq)
{

	/* It's maybe the most important function 
	  * but very simple , since its algorithm is so simple
	  */

	struct task_struct *prev, *next;
	prev = current;
	unsigned long run_time;
	now = sched_clock();
	if (likely((long long)(now - prev->timestamp) < NS_MAX_SLEEP_AVG)) {
		run_time = now - prev->timestamp;
		if (unlikely((long long)(now - prev->timestamp) < 0))
			run_time = 0;
	} else
		run_time = NS_MAX_SLEEP_AVG;

	prev->sleep_avg -= run_time;
	if ((long)prev->sleep_avg <= 0)
		prev->sleep_avg = 0;
	prev->timestamp = prev->last_ran = now;


	 run_time /= (CURRENT_BONUS(prev) ? : 1);

	int cpu = smp_processor_id();

	

	if (unlikely(!rq->nr_running)) {
		//idle_balance(cpu, rq);
		if (!rq->nr_running) {
			next = rq->idle;
			if(!next)
				printk("idle is null\n");
			rq->expired_timestamp = 0;
			//printk("No Processes\n");
			return next;
		}
	}

	

	//rq->expired_timestamp = 0;

	//int inx;
	int new_prio;
	struct list_head *queue;
	

	struct prio_array* from_array = rq->active;
	//if(rq->active->nr_active == 0)
	//{
		//printk("from array is NULL\n");
	//}
	/*if(__prev)
	{
		prev = __prev;
	}
	else	{
		printk("__prev is NULL!\n");
		
	}*/
	//prev = current;
	if(!prev)
		printk("prev is NULL\n");

	

	
	 
	if (unlikely(!from_array->nr_active)) {
		/*
		 * Switch the active and expired arrays.
		 */
		//schedstat_inc(rq, sched_switch);
		rq->active = rq->expired;
		rq->expired = from_array;
		from_array = rq->active;
		rq->expired_timestamp = 0;
		rq->best_expired_prio = MAX_PRIO;
		
	}
	//from_array = rq->active;



	

	int idx = sched_find_first_bit(from_array->bitmap);
	int i = 0;
	struct list_head* p_debug_head = NULL;
	if(idx < 0)
	{
		printk("idx is error Bug here\n");
		//printk("we select idle and rets\n");
		//printk("we try to find the task ready to run withou bitmap\n");
		/*for( i = 100;i<140;i++)
		{
			p_debug_head = &(from_array->queue[i]);
			if(p_debug_head->next){
				printk("We found a usable process index is %d\n",i);
				next = list_entry(p_debug_head->next, struct task_struct, run_list);
				printk("current id is %d pid is %d\n",current->pid,next->pid);
				if(next->pid >0)
				{
					__set_bit(next->prio,from_array->bitmap);
				//	printk("we found the process ,rets it \n");
					goto update_status;
					//return next;
				}

			}
		}
		for(i = 100;i<140;i++){
			p_debug_head = &(rq->expired->queue[i]);
			if(p_debug_head->next){
				printk("We found a usable process expired index is %d\n",i);
				next = list_entry(p_debug_head->next, struct task_struct, run_list);
				printk("current id is %d pid is %d\n",current->pid,next->pid);
				if(next->pid >0)
				{
					__set_bit(next->prio,from_array->bitmap);
					printk("we found the process ,rets it \n");
					return next;
				}

			}
		}

		printk("not find but the nr_active is %d\n",from_array->nr_active);		
		next = rq->idle;
		rq->expired_timestamp = 0;
		return next;*/
	}
		
	queue = from_array->queue + idx;
	if(queue->next == NULL)
	{
		printk("queue->next is NULL\n");
		printk("idx is %d\n",idx);
		printk("from array is %x its active is %d\n",from_array,from_array->nr_active);
	}
	next = list_entry(queue->next, struct task_struct, run_list);
	int now_pid = 0;
update_status:
	now_pid = current->pid;

	if (!rt_task(next) && interactive_sleep(next->sleep_type)) {


		unsigned long long delta = now - next->timestamp;
		
		if (unlikely((long long)(now - next->timestamp) < 0))
			delta = 0;

		if (next->sleep_type == SLEEP_INTERACTIVE)
			delta = delta * (ON_RUNQUEUE_WEIGHT * 128 / 100) / 128;

		from_array = next->array;

		new_prio = recalc_task_prio(next, next->timestamp + delta);

		if (unlikely(next->prio != new_prio)) {

			__dequeue_task_dq_from_source(from_array,next,0);
			next->prio = new_prio;

			__enqueue_task_dq_target(rq, from_array,next,0);
		}



	}

	next->sleep_type = SLEEP_NORMAL;

	prev->sleep_avg -= run_time;
	if ((long)prev->sleep_avg <= 0)
		prev->sleep_avg = 0;
	prev->timestamp = prev->last_ran = now;



	
	//printk("ret from pick_net with rets val is %x\n",next);
	return next;
}

static void put_prev_task_dq(struct rq *rq, struct task_struct *prev)
{
	/* put the prev to the expired array 
	  * my understanding may be wrong
	  * however,do it first :)
	  * It may be left blank since we have done
	  * sth in the pick_next_task_dq?
	  */
	 // printk("put precv task_dq\n");
	  rq = rq;
	 __prev = prev;
}

/* changing of groups/classed is no supported ye*/
#define __MIN_TIMESLICE		max(5 * HZ / 1000, 1)
#define __SCALE_PRIO(x, prio) \
	max(x * (MAX_PRIO - prio) / (MAX_USER_PRIO / 2), __MIN_TIMESLICE)
static void set_curr_task_dq(struct rq *rq)
{
}

static unsigned int static_prio_timeslice(int static_prio)
{
	if (static_prio < NICE_TO_PRIO(0))
		return __SCALE_PRIO(DEF_TIMESLICE * 4, static_prio);
	else
		return __SCALE_PRIO(DEF_TIMESLICE, static_prio);
}

static inline unsigned int task_timeslice(struct task_struct *p)
{
	return static_prio_timeslice(p->static_prio);
}

/*
 * We place interactive tasks back into the active array, if possible.
 *
 * To guarantee that this does not starve expired tasks we ignore the
 * interactivity of a task if the first expired task had to wait more
 * than a 'reasonable' amount of time. This deadline timeout is
 * load-dependent, as the frequency of array switched decreases with
 * increasing number of running tasks. We also ignore the interactivity
 * if a better static_prio task has expired:
 */
static inline int expired_starving(struct rq *rq)
{
	if (rq->curr->static_prio > rq->best_expired_prio)
		return 1;
	if (!STARVATION_LIMIT || !rq->expired_timestamp)
		return 0;
	if (jiffies - rq->expired_timestamp > STARVATION_LIMIT * rq->nr_running)
		return 1;
	return 0;
}


static void task_tick_dq(struct rq *rq, struct task_struct *curr, int queued)
{
	//printk("We are in task_tick_dq \n");
	/*
	  * We got the rq's lock outside 
	  * no need for lock now 
	  */

	struct task_struct *p = curr;
	if (p->array != rq->active) {
		/* Task has expired but was not scheduled yet */
		if(p->pid == 0)
			return;
		set_tsk_need_resched(p);
		//printk("If we got here, We locate the reason of OOps,Well Done\n");
		return;
	}
	
	/*
	 * The task was running during this tick - update the
	 * time slice counter. Note: we do not update a thread's
	 * priority until it either goes to sleep or uses up its
	 * timeslice. This makes it possible for interactive tasks
	 * to use up their timeslices at their highest priority levels.
	 */
	
	if (!--p->time_slice) {
	//	printk("time_slice is over\n");
		__dequeue_task_dq_from_source( rq->active,p,0);
		set_tsk_need_resched(p);
		p->prio = effective_prio(p);
		p->time_slice = task_timeslice(p);
		p->first_time_slice = 0;

		if (!rq->expired_timestamp)
			rq->expired_timestamp = jiffies;
		if (!TASK_INTERACTIVE(p) || expired_starving(rq)) {
			__enqueue_task_dq_target(rq,rq->expired,p,0);
			//printk("enqueue_task_dq1 id is %x to_dq is %x",p->pid,rq->expired);
			if (p->static_prio < rq->best_expired_prio)
				rq->best_expired_prio = p->static_prio;
		} 
		else
		{
			__enqueue_task_dq_target(rq,rq->active,p,0);
			//printk("enqueue_task_dq1 id is %x to_dq is %x",p->pid,rq->active);
		}
	} 
	else {
		/*
		 * Prevent a too long timeslice allowing a task to monopolize
		 * the CPU. We do this by splitting up the timeslice into
		 * smaller pieces.
		 *
		 * Note: this does not mean the task's timeslices expire or
		 * get lost in any way, they just might be preempted by
		 * another task of equal priority. (one with higher
		 * priority would have preempted this task already.) We
		 * requeue this task to the end of the list on this priority
		 * level, which is in essence a round-robin of tasks with
		 * equal priority.
		 *
		 * This only applies to tasks in the interactive
		 * delta range with at least TIMESLICE_GRANULARITY to requeue.
		 */
		if (TASK_INTERACTIVE(p) && !((task_timeslice(p) -
			p->time_slice) % TIMESLICE_GRANULARITY(p)) &&
			(p->time_slice >= TIMESLICE_GRANULARITY(p)) &&
			(p->array == rq->active)) {

			requeue_task(p, rq->active);
			set_tsk_need_resched(p);
		}
	}
}



static void task_fork_dq(struct task_struct *p)
{
//	printk("We are in task_fork_dq \n");
//	printk("We are in task_fork_dq pid is %d\n",p->pid);
	INIT_LIST_HEAD(&p->run_list);
	p->array = NULL;
#if defined(CONFIG_SCHEDSTATS) || defined(CONFIG_TASK_DELAY_ACCT)
	if (unlikely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif

	int cpu = smp_processor_id();

	/*
	 * Share the timeslice between parent and child, thus the
	 * total amount of pending timeslices in the system doesn't change,
	 * resulting in more scheduling fairness.
	 */
	local_irq_disable();
	p->time_slice = (current->time_slice + 1) >> 1;
	/*
	 * The remainder of the first timeslice might be recovered by
	 * the parent if the child exits early enough.
	 */
	p->first_time_slice = 1;
	current->time_slice >>= 1;
	p->timestamp = sched_clock();
	if (unlikely(!current->time_slice)) {
		/*
		 * This case is rare, it happens when the parent has only
		 * a single jiffy left from its timeslice. Taking the
		 * runqueue lock is not a problem.
		 */
		current->time_slice = 1;
		task_tick_dq(cpu_rq(cpu),current,0);
		 
	}
	local_irq_enable();
}

static void prio_changed_dq(struct rq *rq, struct task_struct *p,
			      int oldprio, int running)
{
	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (running) {
		if (p->prio > oldprio)
			resched_task(rq->curr);
	} else
		check_preempt_curr(rq, p, 0);
}

/*
 * We switched to the sched_dq class.
 */
static void switched_to_dq(struct rq *rq, struct task_struct *p,
			     int running)
{
	/*
	 * We were most likely switched from sched_rt, so
	 * kick off the schedule if running, otherwise just see
	 * if we can still preempt the current task.
	 */
	if (running)
		resched_task(rq->curr);
	else
		check_preempt_curr(rq, p, 0);
}

static unsigned int get_rr_interval_dq(struct rq *rq, struct task_struct *task)
{
	/* not deep understood 
	  */
	
	int retval = -EINVAL;

	struct task_struct* p = task;
	if (!p)
		return NULL;

	retval = security_task_getscheduler(p);
	
	return retval;
}


/*
 * Potentially available exiting-child timeslices are
 * retrieved here - this way the parent does not get
 * penalized for creating too many threads.
 *
 * (this cannot be used to 'generate' timeslices
 * artificially, because any timeslice recovered here
 * was given away by the parent in the first place.)
 */
void sched_exit(struct task_struct *p)
{
	unsigned long flags;
	struct rq *rq;

	/*
	 * If the child was a (relative-) CPU hog then decrease
	 * the sleep_avg of the parent as well.
	 */
	rq = task_rq_lock(p->parent, &flags);
	if (p->first_time_slice && task_cpu(p) == task_cpu(p->parent)) {
		p->parent->time_slice += p->time_slice;
		if (unlikely(p->parent->time_slice > task_timeslice(p)))
			p->parent->time_slice = task_timeslice(p);
	}
	if (p->sleep_avg < p->parent->sleep_avg)
		p->parent->sleep_avg = p->parent->sleep_avg /
		(EXIT_WEIGHT + 1) * EXIT_WEIGHT + p->sleep_avg /
		(EXIT_WEIGHT + 1);
	task_rq_unlock(rq, &flags);
}


static const struct sched_class dq_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_dq,
	.dequeue_task		= dequeue_task_dq,
	.yield_task		= yield_task_dq,

	.check_preempt_curr	= check_preempt_wakeup_dq,

	.pick_next_task		= pick_next_task_dq,
	.put_prev_task		= put_prev_task_dq,

	.set_curr_task          = set_curr_task_dq,
	.task_tick		= task_tick_dq,
	.task_fork		= task_fork_dq,

	.prio_changed		= prio_changed_dq,
	.switched_to		= switched_to_dq,

	.get_rr_interval	= get_rr_interval_dq,

};




