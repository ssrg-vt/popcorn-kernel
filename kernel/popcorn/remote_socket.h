#ifndef __KERNEL_POPCORN_REMOTE_SOCKET_H__
#define __KERNEL_POPCORN_REMOTE_SOCKET_H__

#include <popcorn/pcn_kmsg.h>
//struct task_struct;

//int redirect_socket(int family, int type, int protocol);
//int redirect_setsockopt(int fd, int level, int optname, char __user * optval,
//			int optlen);

int process_socket_create(struct pcn_kmsg_message *msg);
int process_setsockopt(struct pcn_kmsg_message *msg);
int process_bind(struct pcn_kmsg_message *msg);
int process_listen(struct pcn_kmsg_message *msg);
int process_accept4(struct pcn_kmsg_message *msg);

#endif
