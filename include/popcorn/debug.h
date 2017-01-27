#ifndef __POPCORN_DEBUG_H__
#define __POPCORN_DEBUG_H__

/*
 * Function macros
 */
#ifdef POPCORN_DEBUG
#define PRINTK(...) printk(__VA_ARGS__)
#else
#define PRINTK(...)
#endif

#define PROCESS_SERVER_VERBOSE  1
#define PROCESS_SERVER_VMA_VERBOSE 0
#define PROCESS_SERVER_NEW_THREAD_VERBOSE 1
#define PROCESS_SERVER_MINIMAL_PGF_VERBOSE 0

#if PROCESS_SERVER_VERBOSE
#define PSPRINTK(...) printk(__VA_ARGS__)
#else
#define PSPRINTK(...) ;
#endif

#if PROCESS_SERVER_VMA_VERBOSE
#define PSVMAPRINTK(...) printk(__VA_ARGS__)
#else
#define PSVMAPRINTK(...) ;
#endif

#if PROCESS_SERVER_NEW_THREAD_VERBOSE
#define PSNEWTHREADPRINTK(...) printk(__VA_ARGS__)
#else
#define PSNEWTHREADPRINTK(...) ;
#endif

#if PROCESS_SERVER_MINIMAL_PGF_VERBOSE
#define PSMINPRINTK(...) printk(__VA_ARGS__)
#else
#define PSMINPRINTK(...) ;
#endif

#endif
