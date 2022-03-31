
#define DO_DEBUG_OUTPUT 0

#ifndef debug_info
#if DO_DEBUG_OUTPUT
#define debug_info(fmt, ...) \
	pr_info(fmt, ##__VA_ARGS__)
#else
#define debug_info(fmt, ...)
#endif
#endif
