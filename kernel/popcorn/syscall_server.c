#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include "syscall_server.h"
#include "types.h"
#include "wait_station.h"

/**
 * Syscalls needed in the kernel
 * */
extern int sys_socket(int family, int type, int protocol);
extern int sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen);
extern int sys_listen(int fd, int backlog);
extern int sys_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag);
extern int sys_setsockopt(int fd, int level, int optname, char __user *optval,
			  int optlen);

int process_remote_syscall(struct pcn_kmsg_message *msg)
{
	int retval = 0;
	syscall_fwd_t *req = (syscall_fwd_t *)msg;
	syscall_rep_t *rep = kmalloc(sizeof(*rep), GFP_KERNEL);

	/*Call the original system call and pass in delivered params. Due to the
	 * way the macro is set up on the remote side, params are filled
	 * backwards. 3 params, the request will go param2 = 1st argument;
	 * param1 = 2nd argument, param0 = 3rd argument. For 2 params, param1 =
	 * 1st argument, param0 = 2nd argument*/
	switch(req->call_type) {
	case PCN_SYSCALL_SOCKET_CREATE:
		/*int family; int type; int protocol*/
		retval = sys_socket((int)req->param2, (int)req->param1,
				    (int)req->param0);
		break;
	case PCN_SYSCALL_SETSOCKOPT:
		retval = sys_setsockopt((int)req->param4, (int)req->param3,
					(int)req->param2,
					(char __user*)req->param1,
					(int)req->param0);
		break;
	case PCN_SYSCALL_BIND:
		retval = sys_bind((int)req->param2, (struct sockaddr __user*)
				  req->param1, (int)req->param0);
		break;
	case PCN_SYSCALL_LISTEN:
		retval = sys_listen((int)req->param1, (int)req->param0);
		break;
	case PCN_SYSCALL_ACCEPT4:
		retval = sys_accept4((int)req->param3,
				     (struct sockaddr __user*)req->param2,
				     (int __user*)req->param1,
				     (int)req->param0);
		break;
	default:
		retval = -EINVAL;
	}
	rep->origin_pid = current->origin_pid;
	rep->remote_ws = req->remote_ws;
	rep->ret = retval;
	pcn_kmsg_send(PCN_KMSG_TYPE_SYSCALL_REP, current->remote_nid, rep,
		      sizeof(*rep));
	kfree(rep);
	return retval;
}

static int handle_syscall_reply(struct pcn_kmsg_message *msg)
{
	syscall_rep_t *rep = (syscall_rep_t *)msg;
	struct wait_station *ws = wait_station(rep->remote_ws);

	ws->private = rep;
	complete(&ws->pendings);
	return 0;
}

DEFINE_KMSG_RW_HANDLER(syscall_fwd, syscall_fwd_t, origin_pid);

int __init syscall_server_init(void)
{
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_FWD,
			      syscall_fwd);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_REP,
			      syscall_reply);
	return 0;
}
