/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 * on Linux 3.12 for only 2 nodes
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/file.h>
//#include <linux/pcn_kmsg.h>
#include <popcorn/pcn_kmsg.h>

#include <linux/fdtable.h>

#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

#include <linux/delay.h>
#include <linux/time.h>

extern int _init_RemoteCPUMask(void);
static int connection_handler(void *arg0);
static int send_thread(void);
static int executer_thread(void* arg0);
static int test_thread(void* arg0);

struct task_struct *handler;
struct task_struct *sender_handler;
struct task_struct *exect_handler;
struct task_struct *test_handler;

enum pcn_connection_status is_connection_done = PCN_CONN_WATING;

int interrupted = 0;

struct sockaddr_in dest_addr;
struct socket *sock_send = NULL;
struct socket *sock_recv, *conn_sock = NULL;
struct sockaddr_in serv_addr;
unsigned int my_ipaddr = 0x0A0101C1;//INADDR_LOOPBACK;
module_param(my_ipaddr, uint, 0);

#define PORT 1234
//#define INADDR_SEND INADDR_LOOPBACK
//#define INADDR_SEND (10<<24 | 0<<16 | 2<<8 | 15)

inline int pcn_connection_status(void)
{
	return is_connection_done;
}

typedef struct _rcv_wait{
	struct list_head list;
	void * msg;
}rcv_wait;

struct semaphore rcv_q_empty;
DEFINE_SPINLOCK(rcv_q_mutex);
static rcv_wait rcv_wait_q;

struct semaphore send_connDone;
struct semaphore rcv_connDone;
struct semaphore pcn_send_sem;

static int __init initialize(void);

unsigned int my_cpu = 0;
static int count = 0;
static int exec_count = 0;

static pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];

/* for debug */
#define TEST_MSG_LAYER 0

#if TEST_MSG_LAYER
#define MSG_LENGTH 64
struct test_msg_t
{
	struct pcn_kmsg_hdr hdr;
	unsigned char payload[MSG_LENGTH];
};

#endif

void enq_rcv(rcv_wait *strc)
{
	spin_lock(&rcv_q_mutex);
	INIT_LIST_HEAD(&(strc->list));
	list_add_tail(&(strc->list), &(rcv_wait_q.list));
	up(&rcv_q_empty);
	spin_unlock(&rcv_q_mutex);
}

rcv_wait * dq_rcv(void)
{
	rcv_wait *tmp;
	down_interruptible(&rcv_q_empty);
	spin_lock(&rcv_q_mutex);
	if(list_empty(&rcv_wait_q.list)){
		printk("List is empty...\n");
		spin_unlock(&rcv_q_mutex);
		return NULL;
	}
	else{
		tmp = list_first_entry (&rcv_wait_q.list, rcv_wait, list);
		list_del(rcv_wait_q.list.next);
		spin_unlock(&rcv_q_mutex);
		return tmp;
	}
}


// Initialize callback table to null, set up control and data channels
int __init initialize()
{
	printk("initialization\n");

	INIT_LIST_HEAD(&rcv_wait_q.list);

	printk("In pcn new messaging layer init\n");
	sema_init(&(rcv_q_empty), 0);

	sema_init(&send_connDone,0);
	sema_init(&rcv_connDone,0);
	sema_init(&pcn_send_sem,1);

	// Sharath:
#if CONFIG_ARM64
        my_cpu = 0;
#else
        my_cpu = 1;
#endif

	//TODO - check all nodes are available - cant be done on sockets ??

	handler = kthread_run(connection_handler, &callbacks, "pcn_recv_thread");
	if(handler < 0){
		printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
		return (long long int)handler;
	}

	sender_handler = kthread_run(send_thread, (void *)NULL, "pcn_send");
	if(sender_handler < 0){
		printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
		return (long long int)sender_handler;
	}

	exect_handler = kthread_run(executer_thread, NULL, "pcn_exec_thread");
	if(exect_handler < 0){
		printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
		return (long long int)exect_handler;
	}

        //down_interruptible(&send_connDone);

#if TEST_MSG_LAYER
	test_handler = kthread_run(test_thread, (void *)NULL, "pcn_test");
	if(test_handler < 0){
		printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
		return (long long int)exect_handler;
	}
#endif /* TEST_MSG_LAYER */

	printk(KERN_INFO "Popcorn Messaging Layer Initialized\n");
	return 0;
}

#if !TEST_MSG_LAYER
late_initcall(initialize);
#endif

#if TEST_MSG_LAYER
struct timeval start[10], end[10];

void handle_selfie_test(struct pcn_kmsg_message* inc_msg)
{
	int i=0;
	struct test_msg_t *msg = (struct test_msg_t *)inc_msg;

	do_gettimeofday(&end[exec_count]);
	//printk("%s", msg->payload);

	if(exec_count == 9)
	{
		for(i=0;i<10;i++)
		{
			printk("Time: %ld\n", ((end[i].tv_sec * 1000000 + end[i].tv_usec)
		  		- (start[i].tv_sec * 1000000 + start[i].tv_usec)));
		}
	}
}

int test_thread(void* arg0)
{
	int i;
	printk("Test function %s: called\n",__func__);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST,
		handle_selfie_test);

	msleep(1000);

	for (i=0;i<10;i++)
	{
		struct test_msg_t *msg;
		int payload_size = MSG_LENGTH;

		msg = (struct test_msg_t *) vmalloc(sizeof(struct test_msg_t));
		msg->hdr.type= PCN_KMSG_TYPE_SELFIE_TEST;
		memset(msg->payload,'b',payload_size);

		do_gettimeofday(&start[i]);
		pcn_kmsg_send_long(1,(struct pcn_kmsg_long_message*)msg, payload_size);
		vfree(msg);
	}

	printk("Finished Testing\n");

	return 0;
}

#endif /* TEST_MSG_LAYER */

int executer_thread(void* arg0)
{
	rcv_wait *wait_data;
	pcn_kmsg_cbftn ftn;

	printk("%s:called\n",__func__);

	while(1)
	{
		wait_data=dq_rcv();

		struct pcn_kmsg_message *msg = wait_data->msg;
		//printk("Executer Thread\n type %d size %d count %d\n",msg->hdr.type,msg->hdr.size, exec_count);
		if(msg->hdr.type < 0 || msg->hdr.type >= PCN_KMSG_TYPE_MAX)
		{
			printk(KERN_INFO "Received invalid message type %d\n", msg->hdr.type);
			vfree(msg);
		}
		else
		{
			ftn = callbacks[msg->hdr.type];
			if(ftn != NULL)
			{
				ftn(msg);
			}else
			{
				printk(KERN_INFO "Recieved message type %d size %d has no registered callback!\n", msg->hdr.type,msg->hdr.size);
				vfree(msg);
			}
		}

		kfree(wait_data);
		exec_count++;

#if TEST_MSG_LAYER

		if(exec_count == 10)
		{
			while(1)
			{
				msleep(10);
				if(kthread_should_stop())
				{
					printk("Exiting executer thread\n");
					return 0;
				}
			}
		}

#endif /*TEST_MSG_LAYER*/

	}
}

int send_thread(void)
{
	int err = 0;

	printk("In MSG LAYER send thread\n");

	err = sock_create(PF_INET, SOCK_STREAM,
		    IPPROTO_TCP, &sock_send);
	if(err < 0)
	{
		printk("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
		return err;
	}

	printk(" successfully created socket\n");

	int val = 1;
	err = sock_send->ops->setsockopt(sock_send, SOL_TCP, TCP_NODELAY,
			(char __user *)&val, sizeof(val));

	printk("Successfully set socket opt\n");

	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(PORT);
	dest_addr.sin_addr.s_addr = htonl(my_ipaddr); //TODO - hardcoding

	do
	{
		err = sock_send->ops->connect(sock_send,(struct sockaddr *) &dest_addr,
	     		sizeof(dest_addr), 0);
		if(err < 0)
		{
			printk("Failed to connect to socket..!! Messaging layer init failed with err %d\n", err);
			msleep(5000);
		}
	}while(err < 0);

    down_interruptible(&rcv_connDone);
	is_connection_done = PCN_CONN_CONNECTED;
	up(&send_connDone);
	printk("############## Connection Done...PCN_SEND Thread: my_ipaddr: 0x%x\n", my_ipaddr);

	// Sharath: For socket based messaging layer
	_init_RemoteCPUMask();

#if TEST_MSG_LAYER
	while(1)
	{
		msleep(10);
		if(kthread_should_stop())
		{
			printk("coming out of send thread\n");
			return 0;
		}
	}
#endif /*TEST_MSG_LAYER*/

	return 0;
}

int connection_handler(void *arg0)
{
        struct pcn_kmsg_message *message, *data, *msg_del, *temp_buff;
        int deficit = 0, msg_recvd = 0;
        rcv_wait *wait_data;
        int err = 0, msg_size=0, temp_len = 0, copy_len = 0;
        int val = 1;

        printk(" IN %s\n", __func__);

        err = sock_create(PF_INET, SOCK_STREAM,
                    IPPROTO_TCP, &sock_recv);
        if(err < 0)
        {
                printk("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
                return err;
        }

        printk(" successfully created socket\n");

        err = sock_recv->ops->setsockopt(sock_recv, SOL_TCP, TCP_NODELAY,
                        (char __user *)&val, sizeof(val));
        if(err < 0)
        {
                printk("Failed to set socket option for receive socket..!! err %d\n", err);
        }

        printk("Successfully set socket opt\n");

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(PORT);

        err = sock_recv->ops->bind(sock_recv, (struct sockaddr *)&serv_addr,
                     sizeof(serv_addr));
        if(err < 0)
        {
                printk("Failed to bind connection..!! Messaging layer init failed\n");
                goto end;
        }

        printk(" successfully bound to socket\n");

        err = sock_recv->ops->listen(sock_recv, 1);
        if(err < 0)
        {
                printk("Failed to listen on connection..!! Messaging layer init failed\n");
                goto end;
        }

        printk(" successfully listening on socket\n");
        err = sock_create(PF_INET, SOCK_STREAM,
                    IPPROTO_TCP, &conn_sock);
        if(err < 0)
        {
                printk("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
                goto end;
        }

        err = conn_sock->ops->setsockopt(conn_sock, SOL_TCP, TCP_NODELAY,
                        (char __user *)&val, sizeof(val));
        if(err < 0)
        {
                printk("Failed to set socket option for accept socket..!! err %d\n", err);
        }

        printk(" successfully set accept socket options\n");

        err = conn_sock->ops->accept(sock_recv, conn_sock, 0);

        if(conn_sock == NULL)
        {
                printk("Failed to accept connection..!! Messaging layer init failed\n");
                goto exit;
        }

        printk(" successfully created accept socket\n");

        up(&rcv_connDone);
        printk(" Recieve connection successfully completed..!!\n");

        /* temperory allocation to handle message less than header size */
        temp_buff = vmalloc(sizeof(struct pcn_kmsg_hdr));
        if(temp_buff==NULL)
        {
                printk("Can't Vmalloc..\n");
                return -1;
        }

        /* buffer to recive data */
        msg_del = (struct pcn_kmsg_message*)vmalloc(128*PAGE_SIZE);
        if(msg_del==NULL)
        {
                printk("Can't Vmalloc..\n");
                return -1;
        }

        while(1)
        {
#if TEST_MSG_LAYER
                if(count == 10)
                {
                        printk("entering while loop\n");

                        while(1)
                        {
                                msleep(10);
                                if(kthread_should_stop())
                                        return 0;
                        }
                }
#else
                if(kthread_should_stop())
                {
                        vfree(msg_del);
                        vfree(temp_buff);
                        return 0;
                }

#endif /*TEST_MSG_LAYER*/

                struct msghdr msg;
                struct iovec iov;
                mm_segment_t oldfs;
                int size = 0, len = 0;

                if (conn_sock->sk==NULL)
                        return 0;

                len = (128*PAGE_SIZE);
                data = msg_del;

                iov.iov_base = data;
                iov.iov_len = len;

                msg.msg_flags = MSG_WAITFORONE;
                msg.msg_name = conn_sock;
                msg.msg_namelen  = sizeof(struct sockaddr_in);
                msg.msg_control = NULL;
                msg.msg_controllen = 0;
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = NULL;

                oldfs = get_fs();
                set_fs(KERNEL_DS);
                size = sock_recvmsg(conn_sock,&msg,len,msg.msg_flags);
                set_fs(oldfs);

                while(size > 0)
                {
                        if(size < sizeof(struct pcn_kmsg_hdr)){
                                printk(KERN_ALERT "msg recieved without header\n");
                                memcpy(temp_buff, data, size);
                                temp_len = size;
                                break;
                        }

                        if(temp_len != 0)
                        {
                                memcpy(temp_buff, data, (sizeof(struct pcn_kmsg_hdr) - temp_len));
                                msg_size = temp_buff->hdr.size;

                                message=vmalloc(msg_size);
                                if(message==NULL)
                                {
                                        printk("Can't Vmalloc..\n");
                                        return -1;
                                }

                                memcpy(message,temp_buff,temp_len);

                                copy_len = (size<(msg_size-temp_len))?size:(msg_size-temp_len);
                                memcpy(message,data,copy_len);
                                data = (void *)data+copy_len;
                        }
                        else if(deficit == 0)
                        {
                                msg_size = data->hdr.size;

                                message=vmalloc(msg_size);
                                if(message==NULL)
                                {
                                        printk("Can't Vmalloc..\n");
                                        return -1;
                                }

                                copy_len = (size<msg_size)?size:msg_size;
                                memcpy(message,data,copy_len);
                                data = (void *)data+copy_len;
                        }
                        else
                        {
                                copy_len = (size<deficit)?size:deficit;
                                memcpy((void *)message+msg_recvd, msg_del, copy_len);
                                data = (void *)data+copy_len;
                        }

                        if(size < (msg_size - temp_len))
                        {
                                deficit = msg_size - size;
                                msg_size = msg_size - copy_len;
                                msg_recvd += size;
                                break;
                        }
                        temp_len = 0;
                        deficit = 0;
                        msg_recvd = 0;
                        wait_data=kmalloc(sizeof(rcv_wait),GFP_KERNEL);
                        wait_data->msg = message;
                        enq_rcv(wait_data);
                        count++;
                        size -= msg_size;
                }
        }
exit:
        sock_release(conn_sock);
        conn_sock = NULL;
end:
        sock_release(sock_recv);
        sock_recv = NULL;
        is_connection_done = PCN_CONN_WATING;
        return err;

}

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	if(type >= PCN_KMSG_TYPE_MAX)
		return -1; //invalid type

	printk("%s: registering %d \n",__func__, type);
	callbacks[type] = callback;
	return 0;
}

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	if(type >= PCN_KMSG_TYPE_MAX)
		return -1;

	printk("Unregistering callback %d\n", type);
	callbacks[type] = NULL;
	return 0;
}

int pcn_kmsg_send(unsigned int dest_cpu, struct pcn_kmsg_message *msg)
{

	if( pcn_connection_status() != PCN_CONN_CONNECTED) {
		printk("PCN_CONNECTION is not yet established\n");
		return -1;
	}

	return pcn_kmsg_send_long(dest_cpu, (struct pcn_kmsg_long_message *)msg,
				sizeof(struct pcn_kmsg_message)-sizeof(struct pcn_kmsg_hdr));
}

int pcn_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size)
{
        struct msghdr msg;
        struct iovec iov;
        mm_segment_t oldfs;
        int size = 0, curr_size = 0;

	if( pcn_connection_status() != PCN_CONN_CONNECTED) {
		printk("PCN_CONNECTION is not yet established\n");
		return -1;
	}

	lmsg->hdr.size = payload_size+sizeof(struct pcn_kmsg_hdr);
	lmsg->hdr.from_cpu = my_cpu;

	if(dest_cpu==my_cpu)
	{
		rcv_wait *exec_data = NULL;
		while(exec_data==NULL)
		{
			exec_data = kmalloc(sizeof(rcv_wait),GFP_KERNEL);
		}
		exec_data->msg=vmalloc(lmsg->hdr.size);
		memcpy(exec_data->msg,lmsg,lmsg->hdr.size);
		enq_rcv(exec_data);
		printk("%s: This is a selfie...\n",__func__);
		return lmsg->hdr.size;
	}

	curr_size = lmsg->hdr.size;

	/* Send the message using socket send */
        if (sock_send->sk==NULL)
           return 0;

	//printk("Coming to msg send\n");
        iov.iov_base = lmsg;
        iov.iov_len = lmsg->hdr.size;

        msg.msg_flags = 0;
        msg.msg_name = sock_send;
        msg.msg_namelen  = sizeof(struct sockaddr_in);
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = NULL;

		down_interruptible(&pcn_send_sem);
        oldfs = get_fs();
        set_fs(KERNEL_DS);

        size = sock_sendmsg(sock_send,&msg,curr_size);
        set_fs(oldfs);
		up(&pcn_send_sem);
	return (size<0?-1:size);

}

inline void pcn_kmsg_free_msg(void *msg){
	kfree(msg);
}

//TODO
inline int pcn_kmsg_get_node_ids(uint16_t *nodes, int len, uint16_t *self){
	//*self = cpumask_first(cpu_present_mask);

#if CONFIG_ARM64
		*self = 0;
#else
		*self = 1;
#endif

	return 0;
}

#if TEST_MSG_LAYER

static void __exit unload(void)
{
	printk("Stopping kernel threads\n");

	printk("in exit: %p %p %p\n", sender_handler, exect_handler, handler);
	if(sender_handler== NULL || exect_handler == NULL || handler == NULL)
	{
		printk("NULL pointer\n");
		return;
	}

	kthread_stop(sender_handler);
	kthread_stop(exect_handler);
	kthread_stop(handler);

	printk("After kthread stop\n");
	printk(" in exit %p %p %p\n", sock_send, sock_recv, conn_sock);

	if(sock_send != NULL) sock_release(sock_send);
	if(sock_recv != NULL) sock_release(sock_recv);
	if(conn_sock != NULL) sock_release(conn_sock);

	sock_send = NULL;
	sock_recv = NULL;

	printk("Successfully unloaded module\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");

#else

EXPORT_SYMBOL(pcn_kmsg_free_msg);
EXPORT_SYMBOL(pcn_kmsg_send_long);
EXPORT_SYMBOL(pcn_kmsg_send);
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);
EXPORT_SYMBOL(pcn_kmsg_register_callback);
EXPORT_SYMBOL(pcn_kmsg_get_node_ids);

#endif /* TEST_MSG_LAYER */
