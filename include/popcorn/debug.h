#ifndef __POPCORN_DEBUG_H__
#define __POPCORN_DEBUG_H__

/*
 * Function macros
 */
#ifdef CONFIG_POPCORN_DEBUG
#define PRINTK(...) printk(__VA_ARGS__)
#else
#define PRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_PROCESS_SERVER_VERBOSE
#define PSPRINTK(...) printk(__VA_ARGS__)
#else
#define PSPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_VMA_SERVER_VERBOSE
#define VSPRINTK(...) printk(__VA_ARGS__)
#else
#define VSPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_PAGE_SERVER_VERBOSE
#define PGSPRINTK(...) printk(__VA_ARGS__)
#else
#define PGSPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#define MSGPRINTK(...) printk(__VA_ARGS__)
#else
#define MSGPRINTK(...)
#endif
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE_DEBUG
#define MSGDPRINTK(...) printk(__VA_ARGS__)
#else
#define MSGDPRINTK(...)
#endif
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_DATA
#define MSGDATA(...) printk(__VA_ARGS__)
#else
#define MSGDATA(...)
#endif

#endif
