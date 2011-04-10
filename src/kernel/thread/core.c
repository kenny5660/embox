/**
 * @file
 * @brief Implementation of methods in api.h
 *
 * @date 22.04.2010
 * @author Dmitry Avdyukhin
 */

#include <assert.h>
#include <errno.h>

#include <embox/unit.h>

#include <util/pool.h>
#include <kernel/critical/api.h>
#include <kernel/thread/api.h>
#include <kernel/thread/sched.h>
#include <hal/context.h>
#include <hal/arch.h>
#include <hal/ipl.h>

/*FIXME this is platform specific*/
#define IDLE_THREAD_STACK_SZ 0x1000

EMBOX_UNIT(unit_init, unit_fini);

struct thread *idle_thread;

struct list_head __thread_list = LIST_HEAD_INIT(__thread_list);

struct thread *thread_alloc(void);
void thread_free(struct thread *t);

/**
 * Function, which does nothing. For idle_thread.
 */
static void *idle_run(void *arg) {
	while (true) {
		thread_yield();
	}
	return NULL;
}

static int unit_init(void) {
	static char idle_thread_stack[IDLE_THREAD_STACK_SZ];
	struct thread *current;

	if (!(current = thread_alloc())) {
		return -ENOMEM;
	}
	thread_init(current, (void *(*)(void *)) -1, (void *) -1, 0);
	current->priority = THREAD_PRIORITY_MAX;

	if (!(idle_thread = thread_alloc())) {
		thread_free(current);
		return -ENOMEM;
	}
	thread_init(idle_thread, idle_run, idle_thread_stack, IDLE_THREAD_STACK_SZ);
	idle_thread->priority = THREAD_PRIORITY_MIN;

	return sched_init(current, idle_thread);
}

static int unit_fini(void) {
	sched_lock();

	return 0;
}

/**
 * Wrapper for thread start routine.
 *
 * @param thread_pointer pointer at thread.
 */
static void __attribute__((noreturn)) thread_run(int ignored) {
	struct thread *current;
	void *arg, *ret;

	assert(!critical_allows(CRITICAL_PREEMPT));

	current = thread_self();

	sched_unlock_noswitch();
	ipl_enable();

	assert(!critical_inside(CRITICAL_PREEMPT));

	arg = current->run_arg;
	ret = current->run(arg);
	current->run_ret = ret;

	thread_stop(current);

	/* NOTREACHED */assert(false);
}

struct thread *thread_init(struct thread *t, void *(*run)(void *),
		void *stack_address, size_t stack_size) {
	struct thread *current;
	char *stack_pointer;
	if (!t) {
		return NULL;
	}

	if (!run || !stack_address) {
		return NULL;
	}

	context_init(&t->context, true);
	context_set_entry(&t->context, thread_run, (int) t);
	stack_pointer = (char*)stack_address + stack_size;
	context_set_stack(&t->context, stack_pointer);

	t->run = run;

	t->state = THREAD_STATE_TERMINATE;
	t->priority = 1;

	INIT_LIST_HEAD(&t->sched_list);
	INIT_LIST_HEAD(&t->messages);
	event_init(&t->msg_event, "msg");
	event_init(&t->event, "finish");
	t->need_message = false;

	if (NULL != (current = thread_self())) {
		t->own_console = current->own_console;
	}

	return t;
}

void thread_start(struct thread *t) {
	sched_add(t);
}

void thread_change_priority(struct thread *t, int priority) {
	sched_lock();

	sched_remove(t);
	t->priority = priority;
	sched_add(t);

	sched_unlock();
}

int thread_stop(struct thread *t) {
	static struct thread *zombie; /* Last zombie thread (if any). */

	if (!t) {
		return -EINVAL;
	}

	if (t == idle_thread || t == zombie) {
		return -EBUSY;
	}

	sched_lock();

	sched_remove(t);

	sched_wake(&t->event);

	sched_unlock();

	return 0;
}

int thread_join(struct thread *t, void **p_ret) {
	assert(t);

	// XXX Eldar what's wrong?
	if (t->state && t->state != THREAD_STATE_TERMINATE) {
		sched_sleep(&t->event);
	}

	if (p_ret) {
		*p_ret = t->run_ret;
	}
	return 0; // TODO thread_join ret value
}

void thread_yield(void) {
	sched_lock();

	thread_self()->reschedule = true;

	sched_unlock();
}

struct thread *thread_lookup(__thread_id_t id) {
	struct thread *t;

	thread_foreach(t) {
		if (t->id == id) {
			return t;
		}
	}

	return NULL;
}

/*FIXME allocation memory*/
POOL_DEF(struct thread, thread_pool, __THREAD_POOL_SZ);

struct thread *thread_alloc(void) {
	struct thread *t;

	if (!(t = (struct thread *) static_cache_alloc(&thread_pool))) {
		return NULL;
	}

	list_add(&t->thread_link, &__thread_list);

	return t;
}

void thread_free(struct thread *t) {
	assert(t != NULL);
	list_del_init(&t->thread_link);
	static_cache_free(&thread_pool, t);
}

