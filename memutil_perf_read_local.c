#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include "linux/perf_event.h"

#define __load_acquire(ptr)                                         \
({                                                                  \
	__unqual_scalar_typeof(*(ptr)) ___p = READ_ONCE(*(ptr));        \
	barrier();                                                      \
	___p;                                                           \
})

enum event_type_t {
	EVENT_FLEXIBLE = 0x1,
	EVENT_PINNED = 0x2,
	EVENT_TIME = 0x4,
	/* see ctx_resched() for details */
	EVENT_CPU = 0x8,
	EVENT_ALL = EVENT_FLEXIBLE | EVENT_PINNED,
};

static inline u64 perf_clock(void)
{
	return local_clock();
}

static inline int is_cgroup_event(struct perf_event *event)
{
	return event->cgrp != NULL;
}

static __always_inline enum perf_event_state
__perf_effective_state(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader;

	if (leader->state <= PERF_EVENT_STATE_OFF)
		return leader->state;

	return event->state;
}

static __always_inline void
__perf_update_times(struct perf_event *event, u64 now, u64 *enabled, u64 *running)
{
	enum perf_event_state state = __perf_effective_state(event);
	u64 delta = now - event->tstamp;

	*enabled = event->total_time_enabled;
	if (state >= PERF_EVENT_STATE_INACTIVE)
		*enabled += delta;

	*running = event->total_time_running;
	if (state >= PERF_EVENT_STATE_ACTIVE)
		*running += delta;
}

static inline
u64 perf_cgroup_event_time_now(struct perf_event *event, u64 now)
{
	struct perf_cgroup_info *t;

	t = per_cpu_ptr(event->cgrp->info, event->cpu);
	if (!__load_acquire(&t->active))
		return t->time;
	now += READ_ONCE(t->timeoffset);
	return now;
}

static u64
perf_event_time_now(struct perf_event *event, u64 now)
{
	struct perf_event_context *ctx = event->ctx;

	if (unlikely(!ctx))
		return 0;

	if (is_cgroup_event(event))
		return perf_cgroup_event_time_now(event, now);

	if (!(__load_acquire(&ctx->is_active) & EVENT_TIME))
		return ctx->time;

	now += READ_ONCE(ctx->timeoffset);
	return now;
}

static void
calc_timer_values(struct perf_event *event, u64 *now, u64 *enabled, u64 *running)
{
	u64 ctx_time;

	*now = perf_clock();
	ctx_time = perf_event_time_now(event, *now);
	__perf_update_times(event, ctx_time, enabled, running);
}

/*
 * NMI-safe method to read a local event, that is an event that
 * is:
 *   - either for the current task, or for this CPU
 *   - does not have inherit set, for inherited task events
 *     will not be local and we cannot read them atomically
 *   - must not have a pmu::count method
 */
int memutil_perf_event_read_local(struct perf_event *event, u64 *value,
			  u64 *enabled, u64 *running)
{
	unsigned long flags;
	int ret = 0;

	/*
	 * Disabling interrupts avoids all counter scheduling (context
	 * switches, timer based rotation and IPIs).
	 */
	local_irq_save(flags);

	/*
	 * It must not be an event with inherit set, we cannot read
	 * all child counters from atomic context.
	 */
	if (event->attr.inherit) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* If this is a per-task event, it must be for current */
	if ((event->attach_state & PERF_ATTACH_TASK) &&
	    event->hw.target != current) {
		ret = -EINVAL;
		goto out;
	}

	/* If this is a per-CPU event, it must be for this CPU */
	if (!(event->attach_state & PERF_ATTACH_TASK) &&
	    event->cpu != smp_processor_id()) {
		ret = -EINVAL;
		goto out;
	}

	/* If this is a pinned event it must be running on this CPU */
	if (event->attr.pinned && event->oncpu != smp_processor_id()) {
		ret = -EBUSY;
		goto out;
	}

	/*
	 * If the event is currently on this CPU, its either a per-task event,
	 * or local to this CPU. Furthermore it means its ACTIVE (otherwise
	 * oncpu == -1).
	 */
	if (event->oncpu == smp_processor_id())
		event->pmu->read(event);

	*value = local64_read(&event->count);
	if (enabled || running) {
		// u64 now = event->shadow_ctx_time + perf_clock();
		// u64 __enabled, __running;
		u64 __enabled, __running, __now;

		// __perf_update_times(event, now, &__enabled, &__running);
		calc_timer_values(event, &__now, &__enabled, &__running);
		if (enabled)
			*enabled = __enabled;
		if (running)
			*running = __running;
	}
out:
	local_irq_restore(flags);

	return ret;
}
