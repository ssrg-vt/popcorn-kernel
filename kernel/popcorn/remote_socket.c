#include <linux/unistd.h>
#include <linux/socket.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include <popcorn/debug.h>
#include "remote_socket.h"
#include "wait_station.h"
#include "types.h"

/*
 * Remote (Slave) side routines:
 * Kernel routine for socket creatation in remote (slave), calling origin (master).
 * Only Origin will execute some "resource sensitve" system calls.
 * */
int redirect_socket(int family, int type, int protocol)
{
	int retval;
	remote_socket_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	remote_socket_response_t *reply = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;
	req->family = family;
	req->type = type;
	req->protocol = protocol;

	SKPRINTK("[%d] family: %d, type: %d, protocol: %d. origin pid: %d\n",
	       current->pid, family, type, protocol, current->origin_pid);

	retval = pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET, 0, req,
			    sizeof(*req));
	//SKPRINTK("ret: %d. ws->id: %d\n", retval, ws->id);
	kfree(req);

	// wait for origin response a ret value, handled by "handle_remote_socket_response()"
	reply = wait_at_station(ws);
	retval = reply->retval;
	SKPRINTK("reply from master: %d\n", retval);

	return retval;
}

int redirect_setsockopt(int fd, int level, int optname, char __user * optval,
			int optlen)
{
	int retval;
	remote_setsockopt_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	remote_socket_response_t *reply = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;

	req->fd = fd;
	req->level = level;
	req->optname = optname;
	req->optval = optval;
	req->optlen = optlen;
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SETSOCKOPT, 0, req,
			    sizeof(*req));
	kfree(req);

	reply = wait_at_station(ws);
	retval = reply->retval;

	return retval;
}

int redirect_bind(int fd, struct sockaddr __user *umyaddr, int addrlen)
{
	int retval;
	remote_socket_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	remote_socket_response_t *reply = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;

	req->fd = fd;
	req->umyaddr = umyaddr;
	req->addrlen = addrlen;
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_BIND, 0, req,
			    sizeof(*req));
	kfree(req);

	reply = wait_at_station(ws);
	retval = reply->retval;

	return retval;
}

int redirect_listen(int fd, int backlog)
{
	int retval;
	remote_socket_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	remote_socket_response_t *reply = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;

	req->fd = fd;
	req->backlog = backlog;
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_LISTEN, 0, req,
			    sizeof(*req));
	kfree(req);

	reply = wait_at_station(ws);
	retval = reply->retval;

	return retval;
}

int redirect_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag)
{
	int retval;
	remote_socket_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	remote_socket_response_t *reply = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;

	req->fd = fd;
	req->umyaddr = upeer_sockaddr;
	req->upeer_addrlen = upeer_addrlen;
	req->backlog = flag;	// reuse some fields of REMOTE_SOCKET_FIELDS
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_ACCEPT4, 0, req,
			    sizeof(*req));
	kfree(req);

	reply = wait_at_station(ws);
	retval = reply->retval;

	return retval;
}


static int handle_remote_socket_response(struct pcn_kmsg_message *msg)
{
	remote_socket_response_t *res = (remote_socket_response_t *)msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	SKPRINTK("response ret: %d, ws id: %d\n", res->retval, res->remote_ws);
	//SKPRINTK("ws->pendings %d\n", ws->pendings.done);
	ws->private = res;
	complete(&ws->pendings);
	//SKPRINTK("ws->pendings %d\n", ws->pendings.done);
	return 0;
}


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
/*
 * Origin (Master) side routines:
 * Socket server: handler for "remote (slave) socket create"
 * */
int process_socket_create(struct pcn_kmsg_message *msg)
{
	int retval;
	remote_socket_t *req = (remote_socket_t *)msg;
	remote_socket_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);

	// do socket() in current context
	SKPRINTK("[%d] slave socket created. family: %d, type: %d, protocol: %d\n",
	       current->pid, req->family, req->type, req->protocol);
	//SKPRINTK("req->origin_pid %d, req->remote_ws %d\n",
	//       req->origin_pid, req->remote_ws);

	// do the socket system call on origin side (master)
	retval = sys_socket(req->family, req->type, req->protocol);
	SKPRINTK("[%d] sys_socket ret: %d. worker: %d, remote: %d, current nid: %d\n",
	       current->pid, retval, current->is_worker, current->at_remote,
	       current->remote_nid);

	// response the ret value of sys_socket()
	res->origin_pid = current->origin_pid;
	res->remote_ws = req->remote_ws;  // pass the "ws id" back, to continue execution
	res->retval = retval;
	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE, current->remote_nid,
		      res, sizeof(*res));
	kfree(res);

	return retval;
}

int process_setsockopt(struct pcn_kmsg_message *msg)
{
	remote_setsockopt_t *req = (remote_setsockopt_t *)msg;
	int retval = sys_setsockopt(req->fd, req->level, req->optname,
				    req->optval, req->optlen);
	remote_socket_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);

	res->origin_pid = current->origin_pid;
	res->remote_ws = req->remote_ws;
	res->retval = retval;

	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE, current->remote_nid,
		      res, sizeof(*res));
	kfree(res);

	return retval;
}

int process_bind(struct pcn_kmsg_message *msg)
{
	int retval;
	remote_socket_t *req = (remote_socket_t *)msg;
	remote_socket_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);

	retval = sys_bind(req->fd, req->umyaddr, req->addrlen);

	res->origin_pid = current->origin_pid;
	res->remote_ws = req->remote_ws;
	res->retval = retval;

	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE, current->remote_nid,
		      res, sizeof(*res));
	kfree(res);

	return retval;
}

int process_listen(struct pcn_kmsg_message *msg)
{
	int retval;
	remote_socket_t *req = (remote_socket_t *)msg;
	remote_socket_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);

	retval = sys_listen(req->fd, req->backlog);

	res->origin_pid = current->origin_pid;
	res->remote_ws = req->remote_ws;
	res->retval = retval;

	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE, current->remote_nid,
		      res, sizeof(*res));
	kfree(res);

	return retval;
}

int process_accept4(struct pcn_kmsg_message *msg)
{
	int retval;
	remote_socket_t *req = (remote_socket_t *)msg;
	remote_socket_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);

	retval = sys_accept4(req->fd, req->umyaddr, req->upeer_addrlen,
			     req->backlog);

	res->origin_pid = current->origin_pid;
	res->remote_ws = req->remote_ws;
	res->retval = retval;

	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE, current->remote_nid,
		      res, sizeof(*res));
	kfree(res);

	return retval;
}

/*
 * complete the waiting task (invoking request_remote_work()), continue execute
 * '__process_remote_works' switch loop
 * */
DEFINE_KMSG_RW_HANDLER(socket_create, remote_socket_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(setsockopt, remote_setsockopt_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(bind, remote_socket_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(listen, remote_socket_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(accept4, remote_socket_t, origin_pid);

/**
 * Initialize the remote socket server. Register the socket handlers.
 */
int __init remote_socket_server_init(void)
{
	// when msg comes, call handle_socket_create(), then wake up
	// remote_work_pended completiona in __process_remote_works()
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_SOCKET, socket_create);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_SETSOCKOPT, setsockopt);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_BIND, bind);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_LISTEN, listen);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_ACCEPT4, accept4);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE,
			      remote_socket_response);
	return 0;
}
