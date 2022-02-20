#include <stdio.h>

double stopwatch_elapsed(struct timeval *start, struct timeval *end)
{
	return (double)((end->tv_sec * 1e6 + end->tv_usec) - 
		(start->tv_sec * 1e6 + start->tv_usec)) / 1e6;
}

#ifdef _VERBOSE
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#define false 0
#define true 1
