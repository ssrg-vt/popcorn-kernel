#ifndef __KERNEL_POPCORN_REMOTE_SOCKET_H__
#define __KERNEL_POPCORN_REMOTE_SOCKET_H__


int redirect_socket(int family, int type, int protocol);
int redirect_setsockopt(int fd, int level, int optname, char __user * optval,
			int optlen);
int redirect_bind(int fd, struct sockaddr __user *umyaddr, int addrlen);
int redirect_listen(int fd, int backlog);
int redirect_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag);
long redirect_shutdown(int, int);
long redirect_recvfrom(int, void __user *, size_t, unsigned,
				struct sockaddr __user *, int __user *);

long redirect_epoll_create1(int flags);
long redirect_epoll_ctl(int epfd, int op, int fd,
				struct epoll_event __user *event);
long redirect_epoll_wait(int epfd, struct epoll_event __user *events,
				int maxevents, int timeout);
long redirect_epoll_pwait(int epfd, struct epoll_event __user *events,
				int maxevents, int timeout,
				const sigset_t __user *sigmask,
				size_t sigsetsize);
long redirect_select(int n, fd_set __user *inp, fd_set __user *outp,
			fd_set __user *exp, struct timeval __user *tvp);

long redirect_read(unsigned int fd, char __user *buf, size_t count);
long redirect_write(unsigned int fd, const char __user *buf, size_t count);
long redirect_open(const char __user *filename, int flags, umode_t mode);
long redirect_close(unsigned int fd);
long redirect_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);
long redirect_writev(unsigned long fd,
			   const struct iovec __user *vec,
			   unsigned long vlen);
long redirect_fstat(unsigned int fd,
			struct stat __user *statbuf);
long redirect_sendfile64(int out_fd, int in_fd,
			       loff_t __user *offset, size_t count);
long redirect_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg);
long redirect_fstatat(int dfd, const char __user *filename,
			       struct stat __user *statbuf, int flag);
long redirect_getpid(int dummy);
#endif
