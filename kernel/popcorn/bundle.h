#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_BUNDLE_ID 32

enum {
	BUNDLE_NODE_X86 = 0,
	BUNDLE_NODE_ARM = 1,
	BUNDLE_NODE_UNKNOWN = 2,
};

int setup_bundle_node(void);

struct bundle {
	unsigned int id;
	unsigned int subid;
	unsigned int type;
	unsigned long bundles_online[BITS_TO_LONGS(MAX_BUNDLE_ID)];
};

struct bundle bundle;

#endif
