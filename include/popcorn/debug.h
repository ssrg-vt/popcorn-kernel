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


#define PROCESS_SERVER_VERBOSE 1

#if PROCESS_SERVER_VERBOSE
#define PSPRINTK(...) printk(__VA_ARGS__)
#else
#define PSPRINTK(...)
#endif


#define VMA_SERVER_VERBOSE 0

#if VMA_SERVER_VERBOSE
#define PSVMAPRINTK(...) printk(__VA_ARGS__)
#else
#define PSVMAPRINTK(...)
#endif


#define PAGE_SERVER_VERBOSE 0

#if PAGE_SERVER_VERBOSE
#define PGPRINTK(...) printk(__VA_ARGS__)
#else
#define PGPRINTK(...)
#endif


#define POPCORN_MSG_LAYER_VERBOSE 0        //Jack
#define POPCORN_MSG_LAYER_DEBUG_VERBOSE 0   //Jack

#if POPCORN_MSG_LAYER_VERBOSE
#define MSGPRINTK(...) printk(__VA_ARGS__)
#else
#define MSGPRINTK(...)
#endif
#if POPCORN_MSG_LAYER_DEBUG_VERBOSE
#define MSGDPRINTK(...) printk(__VA_ARGS__)
#else
#define MSGDPRINTK(...)
#endif

#endif
