#ifndef __INCLUDE_POPCORN_DEBUG_H__
#define __INCLUDE_POPCORN_DEBUG_H__

#define PCNPRINTK(...) printk(KERN_INFO "popcorn: " __VA_ARGS__)
#define PCNPRINTK_ERR(...) printk(KERN_ERR "popcorn: " __VA_ARGS__)

/*
 * Function macros
 */
#ifdef CONFIG_POPCORN_DEBUG
#define PRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_PROCESS_SERVER
#define PSPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PSPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_VMA_SERVER
#define VSPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define VSPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_PAGE_SERVER
#define PGPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PGPRINTK(...)
#endif


#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER
#define MSGPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define MSGPRINTK(...)
#endif

#ifdef CONFIG_POPCORN_DEBUG_SYSCALL_SERVER
#define SSPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SSPRINTK(...)
#endif

#ifdef CONFIG_POPCORN_DEBUG_MVX_SERVER
#define MVXPRINTK(...) printk(KERN_INFO "[MVX] "__VA_ARGS__)
#define MVXPRINTK_PRE(...) printk(KERN_INFO "[MVX pre syscall] "__VA_ARGS__)
#define MVXPRINTK_POST(...) printk(KERN_INFO "[MVX post syscall] "__VA_ARGS__)
#else
#define MVXPRINTK(...)
#define MVXPRINTK_PRE(...)
#define MVXPRINTK_POST(...)
#endif

#endif /*  __INCLUDE_POPCORN_DEBUG_H__ */
