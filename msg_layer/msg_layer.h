#ifndef __POPCORN_MSG_LAYER_H__
#define __POPCORN_MSG_LAYER_H__

#include <linux/pcn_kmsg.h>

extern send_cbftn send_callback;
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];

#endif
