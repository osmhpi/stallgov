#include "memutil_perf_counter.h"
#include "pmu_cpuid_helper.h"
#include "pmu_events.h"
#include "memutil_debug_log.h"

#define PARSE_EVENT_MAX_PAIRS 8

typedef char* key_value_pair_t[2];

static struct pmu_events_map *events_map = NULL;

struct PortableEvent {
	char* name;
	u32 type;
	u64 config;
};

static struct PortableEvent portable_events[] = {
	{
		.name = "cycles",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CPU_CYCLES
	},
	{
		.name = "instructions",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_INSTRUCTIONS
	},
	{
		.name = "cache-references",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CACHE_REFERENCES
	},
	{
		.name = "cache-misses",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CACHE_MISSES
	},
	{
		.name = "branch-instructions",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS
	},
	{
		.name = "branch-misses",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_BRANCH_MISSES
	},
	{
		.name = "bus-cycles",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_BUS_CYCLES
	},
	{
		.name = "stalled-cycles-frontend",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_STALLED_CYCLES_FRONTEND
	},
	{
		.name = "stalled-cycles-backend",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_STALLED_CYCLES_BACKEND
	},
	{
		.name = "ref-cycles",
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_REF_CPU_CYCLES
	},
	{
		.name = "cpu-clock",
		.type = PERF_TYPE_SOFTWARE,
		.config = PERF_COUNT_SW_CPU_CLOCK
	},
	{
		.name = "l1d-read",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "l1d-read-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = "l1d-write",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "l1d-write-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = "l1i-read",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "l1i-read-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = "l1i-write",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "l1i-write-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = "ll-read",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "ll-read-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = "ll-write",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)
	},
	{
		.name = "ll-write-miss",
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
	},
	{
		.name = NULL,
		.type = 0,
		.config = 0
	}
};

int memutil_setup_events_map(void)
{
	int i, return_value;
	char* cpuid = memutil_get_cpuid_str();
	debug_info("Memutil: Setting up events map");
	if (!cpuid) {
		pr_warn("Memutil: Failed to read CPUID");
		return -1;
	}
	i = 0;
	for (;;) {
		events_map = &memutil_pmu_events_map[i++];
		if (!events_map->table) {
			pr_warn("Memutil: Did not find pmu events map for CPUID=\"%s\"", cpuid);
			events_map = NULL;
			return_value = -2;
			break;
		}

		if (!memutil_strcmp_cpuid_str(events_map->cpuid, cpuid)) {
			debug_info("Memutil: Found table \"%s\" for CPUID=\"%s\"", events_map->cpuid, cpuid);
			return_value = 0;
			break;
		}
	}
	kfree(cpuid);
	return return_value;
}

void memutil_teardown_events_map(void)
{
	events_map = NULL;
}

static struct pmu_event *find_event(const char *event_name)
{
	struct pmu_event *event = NULL;
	int i = 0;
	if(!events_map)
		return NULL;
	for (;;) {
		event = &events_map->table[i++];
		if (!event->name && !event->event && !event->desc) {
			event = NULL;
			break;
		}
		if (event->name && !strcmp(event->name, event_name)) {
			break;
		}
	}
	return event;
}

static int parse_event_pair(char** pair, u64 *config, u64 *period)
{
	unsigned long long value;
	if (kstrtoull(pair[1], 0, &value)) {
		pr_err("Memutil: parse_event_pair: Converting number string failed. Str=%s", pair[1]);
		return -1;
	}

	//See arch/x86/events/perf_event.h struct x86_pmu_config for what bits are what or Intel Volume 3B documentation
	if (!strcmp("event", pair[0])) {
		*config |= (value & 0xFF);
	} else if (!strcmp("umask", pair[0])) {
		*config |= (value & 0xFF) << 8;
	} else if (!strcmp("cmask", pair[0])) {
		*config |= (value & 0xFF) << 24;
	} else if (!strcmp("edge", pair[0])) {
		*config |= (value & 1) << 18;
	} else if (!strcmp("inv", pair[0])) {
		*config |= (value & 1) << 23;
	} else if (!strcmp("any", pair[0])) {
		pr_warn("Memutil: parse_event_pair: Any config value is used");
		*config |= (value & 1) << 21;
	} else if (!strcmp("period", pair[0])) {
		*period = value;
	} else {
		pr_err("Memutil: parse_event_pair: Unknown key: %s", pair[0]);
		return -2;
	}
	return 0;
}

static int parse_event(struct pmu_event *event, u64 *config, u64 *period)
{
	key_value_pair_t pairs[PARSE_EVENT_MAX_PAIRS];
	size_t event_string_size;
	char *event_string;
	int pair_count;
	int return_value;
	bool is_reading_key = true;
	int pair_index = 0;
	int current_index = 0;

	*config = 0;

	event_string_size = strlen(event->event)+1;
	event_string = kmalloc(event_string_size, GFP_KERNEL);
	if (!event_string) {
		pr_err("Memutil: Failed to allocate memory in parse_event");
		return -1;
	}
	strncpy(event_string, event->event, event_string_size);

	pairs[0][0] = event_string;
	for (; event_string[current_index] != 0; ++current_index) {
		if (is_reading_key && event_string[current_index] == '=') {
			event_string[current_index] = 0;
			++current_index;
			pairs[pair_index][1] = event_string + current_index;
			is_reading_key = false;
		} else if (!is_reading_key && event_string[current_index] == ',') {
			event_string[current_index] = 0;
			++current_index;
			++pair_index;
			if (pair_index >= PARSE_EVENT_MAX_PAIRS) {
				pr_err("Memutil: parse event: more pairs than expected: Expected max %d, event string is %s",
				       PARSE_EVENT_MAX_PAIRS, event_string);
				goto err;
			}
			pairs[pair_index][0] = event_string + current_index;
			is_reading_key = true;
		}
	}
	pair_count = pair_index + 1;

	for (pair_index = 0; pair_index < pair_count; ++pair_index) {
		if (parse_event_pair(pairs[pair_index], config, period)) {
			goto err;
		}
	}

	return_value = 0;
	goto out;
err:
	return_value = -1;
out:
	kfree(event_string);
	return return_value;
}

static struct perf_event * memutil_allocate_perf_counter_for(unsigned int cpu, u32 perf_event_type, u64 perf_event_config)
{
	struct perf_event_attr perf_attr;
	struct perf_event *perf_event;

	memset(&perf_attr, 0, sizeof(perf_attr));

	perf_attr.type = perf_event_type;
	perf_attr.size = sizeof(perf_attr);
	perf_attr.config = perf_event_config;
	perf_attr.disabled = 0; // enable the event by default
	perf_attr.exclude_kernel = 1;
	perf_attr.exclude_hv = 1;

	debug_info("Memutil: Perf create kernel counter");
	perf_event = perf_event_create_kernel_counter(
		&perf_attr,
		cpu,
		/*task*/ NULL,
		/*overflow_handler*/ NULL,
		/*context*/ NULL);

	return perf_event;
}

static bool find_portable_event(const char *event_name, u32 *type, u64 *config)
{
	int i;
	for (i = 0; portable_events[i].name != NULL; ++i) {
		if (!strcmp(event_name, portable_events[i].name)) {
			debug_info("Found portable event %s at index %d", event_name, i);
			*type = portable_events[i].type;
			*config = portable_events[i].config;
			return true;
		}
	}
	debug_info("%s is not a defined portable event", event_name);
	return false;
}


/**
 * @brief Allocate a named counter that is available in the pmu_events table.
 *        This currently only works for Intel CPUs
 */
static struct perf_event *memutil_allocate_named_perf_counter(unsigned int cpu, const char *counter_name)
{
	u32 perf_event_type;
	u64 perf_event_config;
	u64 perf_event_period;
	struct pmu_event *event;

	if (find_portable_event(counter_name, &perf_event_type, &perf_event_config)) {
		return memutil_allocate_perf_counter_for(cpu, perf_event_type, perf_event_config);
	}

	perf_event_type = 4; //hardcoded type that is usually cpu pmu
	perf_event_config = 0;
	perf_event_period = 0;

	debug_info("Memutil: Perf counter searching %s", counter_name);
	event = find_event(counter_name);
	if (!event) {
		pr_warn("Memutil: Failed to find event for given perf counter name \"%s\"", counter_name);
		return ERR_PTR(-1);
	}
	debug_info("Memutil: Perf counter parsing %s", counter_name);
	if (parse_event(event, &perf_event_config, &perf_event_period)) {
		pr_warn("Memutil: Failed to parse event for given perf counter name \"%s\"", counter_name);
		return ERR_PTR(-1);
	}
	debug_info("Memutil: Perf counter allocating %s", counter_name);
	return memutil_allocate_perf_counter_for(cpu, perf_event_type, perf_event_config);
}

long memutil_allocate_perf_counters_for_cpu(unsigned int cpu, char **event_names, struct perf_event **events_array, int event_count)
{
	int i;
	struct perf_event *perf_event;

	debug_info("Memutil: Allocating perf counters");
	for (i = 0; i < event_count; ++i) {
		debug_info("Memutil: Allocate perf counter for event_name%d=\"%s\"", i+1, event_names[i]);
		perf_event = memutil_allocate_named_perf_counter(
			cpu,
			event_names[i]);
		if(unlikely(IS_ERR(perf_event))) {
			pr_err("Memutil: Failed to allocate perf counter for event_name%d=\"%s\": err %pe", i+1, event_names[i], perf_event);
			goto cleanup;
		}
		events_array[i] = perf_event;
	}
	debug_info("Memutil: Allocated perf counters");
	return 0;

cleanup:
	for (--i; i >= 0; --i) { //counter should start with i - 1 --> --i as initializer
		perf_event_release_kernel(events_array[i]);
		events_array[i] = NULL;
	}
	return PTR_ERR(perf_event);
}

void memutil_release_perf_events(struct perf_event **events_array, int array_size)
{
	int i;
	for (i = 0; i < array_size; ++i) {
		if (unlikely(!events_array[i])) {
			pr_warn("Memutil: Tried to release event %d which is NULL", i);
		} else {
			perf_event_release_kernel(events_array[i]);
		}
	}
}
