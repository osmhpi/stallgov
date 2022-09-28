// SPDX-License-Identifier: GPL-2.0-only
/*
 * memutil_perf_counter.c
 *
 * Implementation file for perf counter functionality. This allows allocating perf counters
 * specifying either a portable event name (like cycles, instructions) or a
 * platform specific event name that is specified in "pmu_events.c".
 *
 * Copyright (C) 2021-2022 Leon Matthes, Maximilian Stiede, Erik Griese
 *
 * Authors: Leon Matthes, Maximilian Stiede, Erik Griese
 */

#include "memutil_perf_counter.h"
#include "memutil_cpuid_helper.h"
#include "pmu_events.h"
#include "memutil_printk_helper.h"

/**
 * Maximum amount of key value pairs that can be encountered when parsing the
 * event string of a platform specific event. See the method parse_platform_event
 */
#define PARSE_EVENT_MAX_PAIRS 8

typedef char* key_value_pair_t[2];

/**
 * event map that is used for this platforms cpu
 */
static struct pmu_events_map *events_map = NULL;

/**
 * struct PortableEvent - defines a portable perf event, i.e. a perf event that
 *                        is always available using a specific perf_event_open
 *                        event type and predefined configuration value
 *
 * @name: The name for indentifying this event
 * @type: The perf_event_open type for the event
 * @config: The perf_event_open config for the event
 */
struct PortableEvent {
	char* name;
	u32 type;
	u64 config;
};

/**
 * Array of portable events that should be available on every platform
 */
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
	//mark the end of the map
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
	if (IS_ERR(cpuid)) {
		pr_warn("Memutil: Failed to read CPUID");
		return PTR_ERR(cpuid);
	}
	i = 0;
	for (;;) {
		events_map = &memutil_pmu_events_map[i++];
		if (!events_map->table) {
			//to mark the end of the map their is an entry that is
			//all 0. We reached that entry here, i.e. we reached the
			//end of the map
			pr_warn("Memutil: Did not find pmu events map for CPUID=\"%s\"", cpuid);
			events_map = NULL;
			return_value = -EINVAL;
			break;
		}

		if (!memutil_cpuid_matches(events_map->cpuid, cpuid)) {
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

/**
 * find_platform_event - Find the event with the given name in the table of platform
 *                       specific events. The event map has to be setup for this
 *                       function to work.
 *
 *                       Returns a pointer to the event data on success, otherwise
 *                       NULL is returned.
 * @event_name: The name of the event to find.
 */
static struct pmu_event *find_platform_event(const char *event_name)
{
	struct pmu_event *event = NULL;
	int i = 0;
	if(!events_map) {
		return NULL;
	}
	for (;;) {
		event = &events_map->table[i++];
		if (!event->name && !event->event && !event->desc) {
			//we reached the end of the table that is marked
			//with an entry that has zero for the name,
			//the event string and the description
			event = NULL;
			break;
		}
		if (event->name && !strcmp(event->name, event_name)) {
			break;
		}
	}
	return event;
}

/**
 * parse_event_pair - Parse one key-value pair of a platform specific event string.
 *
 *                    On success 0 is returned, otherwise an error code is returned.
 * @pair: The key value pair to parse, possible keys are:
 *         - event
 *         - umask
 *         - cmask
 *         - edge
 *         - inv
 *         - any
 *         - period
 *        For more information on their meaning see arch/x86/events/perf_event.h
 *        struct x86_pmu_config and the Intel Volume 3B documentation
 *
 *        An example key value pair would be key="event", value="0x2"
 * @config: The config value that should be updated depending on the key value pair
 * @period: The period value that should be updated depending on the key value pair
 */
static int parse_event_pair(char** pair, u64 *config, u64 *period)
{
	int return_value;
	unsigned long long value;
	return_value = kstrtoull(pair[1], 0, &value);
	if (return_value) {
		pr_err("Memutil: parse_event_pair: Converting number string failed. Str=%s", pair[1]);
		return return_value;
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
		//warn as this means we do a measurement not just for the logical
		//core that programmed the event which we might assume.
		pr_warn("Memutil: parse_event_pair: Any config value is used: "
			"Measurement is done across logical cores");
		*config |= (value & 1) << 21;
	} else if (!strcmp("period", pair[0])) {
		*period = value;
	} else {
		pr_err("Memutil: parse_event_pair: Unknown key: %s", pair[0]);
		return -EINVAL;
	}
	return 0;
}

/**
 * parse_platform_event - Parse the data specifying a platform specific event and
 *                        populate the config and period value needed for allocating
 *                        the event with perf_event_open
 *
 *                        Returns 0 on success, otherwise an error code is returned.
 * @event: The data of the event
 * @config: Pointer to which the config for perf_event_open should be written
 * @period: Pointer to which the period for perf_event_open should be written
 */
static int parse_platform_event(struct pmu_event *event, u64 *config, u64 *period)
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
	*period = 0;

	event_string_size = strlen(event->event)+1;
	event_string = kmalloc(event_string_size, GFP_KERNEL);
	if (!event_string) {
		pr_err("Memutil: Failed to allocate memory in parse_platform_event");
		return -ENOMEM;
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
				return_value = -EINVAL;
				goto out;
			}
			pairs[pair_index][0] = event_string + current_index;
			is_reading_key = true;
		}
	}
	pair_count = pair_index + 1;

	for (pair_index = 0; pair_index < pair_count; ++pair_index) {
		return_value = parse_event_pair(pairs[pair_index], config, period);
		if (return_value) {
			goto out;
		}
	}

	return_value = 0;
out:
	kfree(event_string);
	return return_value;
}

/**
 * memutil_allocate_perf_counter_for - Allocate a perf counter with the
 *                                     given type and config for the given cpu.
 *
 *                                     Returns the allocated perf event, which
 *                                     is an error pointer (see IS_ERR) if
 *                                     some error occurred.
 * @cpu: Cpu for which the counter should be allocated
 * @perf_event_type: The type of perf event to allocate
 *                   (value will be directly used for perf_event_open)
 * @perf_event_config: Config for the perf event (value will be directly used for
 *                     perf_event_open)
 */
static struct perf_event * memutil_allocate_perf_counter_for(unsigned int cpu, u32 perf_event_type, u64 perf_event_config)
{
	struct perf_event_attr perf_attr;
	struct perf_event *perf_event;

	memset(&perf_attr, 0, sizeof(perf_attr));

	perf_attr.type = perf_event_type;
	perf_attr.size = sizeof(perf_attr);
	perf_attr.config = perf_event_config;
	perf_attr.disabled = 0; // enable the event by default
	perf_attr.exclude_kernel = 0;

	//we do not want to deal with the hypervisor in any way
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

/**
 * find_portable_event - Try to find a portable event with the given name and initialize
 *                       the type and config data with the portable event's data
 *
 *                       Returns true if a portable event with the given name is
 *                       found, otherwise false is returned.
 * @event_name: Name of the portable event to search for
 * @type: Pointer to value where the type of the portable event should be written
 *        (if the portable event is found)
 * @config: Pointer to value where the config of the portable event should be written
 *          (if the portable event is found)
 */
static bool find_portable_event(const char *event_name, u32 *type, u64 *config)
{
	int i;
	for (i = 0; portable_events[i].name != NULL; ++i) { //the end of the portable events array is marked by an entry with NULL for the name
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
 * memutil_allocate_named_perf_counter - Allocate a counter by specifying its name.
 *                                       The name can either be a portable counter name
 *                                       (see the comment in the header
 *                                       memutil_perf_counter.h) or the name of
 *                                       a platform specifc counter.
 *
 *                                       On success a pointer to the allocated
 *                                       counter is returned. On failure an error
 *                                       pointer (see IS_ERR etc.) is returned.
 * @cpu: Cpu for which the counter should be allocated
 * @counter_name: Name of the counter
 */
static struct perf_event *memutil_allocate_named_perf_counter(unsigned int cpu, const char *counter_name)
{
	int return_value;
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
	event = find_platform_event(counter_name);
	if (!event) {
		pr_warn("Memutil: Failed to find event for given perf counter name \"%s\"", counter_name);
		return ERR_PTR(-EINVAL);
	}
	debug_info("Memutil: Perf counter parsing %s", counter_name);
	return_value = parse_platform_event(event, &perf_event_config, &perf_event_period);
	if (return_value) {
		pr_warn("Memutil: Failed to parse event for given perf counter name \"%s\"", counter_name);
		return ERR_PTR(return_value);
	}
	debug_info("Memutil: Perf counter allocating %s", counter_name);
	return memutil_allocate_perf_counter_for(cpu, perf_event_type, perf_event_config);
}

int memutil_allocate_perf_counters_for_cpu(unsigned int cpu, char **event_names, struct perf_event **events_array, int event_count)
{
	int i;
	struct perf_event *perf_event;

	debug_info("Memutil: Allocating perf counters");
	for (i = 0; i < event_count; ++i) {
		debug_info("Memutil: Allocate perf counter for event_name%d=\"%s\"", i+1, event_names[i]);
		perf_event = memutil_allocate_named_perf_counter(
			cpu,
			event_names[i]);
		if(IS_ERR(perf_event)) {
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
		if (!events_array[i]) {
			pr_warn("Memutil: Tried to release event %d which is NULL", i);
		} else {
			perf_event_release_kernel(events_array[i]);
		}
	}
}
