#ifndef __KERNEL_POPCORN_REMOTE_SOCKET_H__
#define __KERNEL_POPCORN_REMOTE_SOCKET_H__

//struct task_struct;

int redirect_socket(int family, int type, int protocol);
int redirect_setsockopt(int fd, int level, int optname, char __user * optval,
			int optlen);
int redirect_bind(int fd, struct sockaddr __user *umyaddr, int addrlen);
int redirect_listen(int fd, int backlog);
int redirect_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag);
#endif
