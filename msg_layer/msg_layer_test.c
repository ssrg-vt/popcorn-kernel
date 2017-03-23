/*
 * Copyright (C) 2017 JackChuang <horenc@vt.edu>
 * 
 * Testing msg_layer    - general send recv
 *                      - large data (>=8K)
 *                      - (rdma read/write)
 */
#include <linux/module.h>

#include <linux/proc_fs.h>

#include <linux/slab.h>
//#include <linux/ktime.h> 
//#include <linux/time.h> 
#include <linux/delay.h> 
#include <linux/kthread.h>

#include <popcorn/debug.h>
#include <popcorn/pcn_kmsg.h>

#include "common.h"

// for testing rdma read (test2)
int g_test_remote_len = 4*1024; // testing size for RDMA, mimicing user buf size // for rdma dbg
char *g_test_buf; // mimicing user buf

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback) 
{
    if (type >= PCN_KMSG_TYPE_MAX)
        return -ENODEV; /* invalid type */

    DEBUG_LOG_V("%s: registering %d\n",__func__, type);
    callbacks[type] = callback; 
    return 0;
}

/* example - data structure */
typedef struct {
    struct pcn_kmsg_hdr hdr; /* must followd */
    //struct pcn_kmsg_rdma_hdr hdr; /* must followd */
    /* you define */
    int example1;
    int example2;
    char msg[1024]; // testing lager than 1 MTU (cureent MAX is 12k)
}__attribute__((packed)) remote_thread_first_test_request_t; // for cache

/* example - handler */
static void handle_remote_thread_first_test_request(
                                        struct pcn_kmsg_long_message* inc_lmsg)
{
    remote_thread_first_test_request_t* request = 
                        (remote_thread_first_test_request_t*) inc_lmsg;
    
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
    EXP_LOG("<<< TEST1: my_nid=%d t %lu "
                            "example1(from)=%d example2=%d (good) >>>\n", 
                            my_nid, request->hdr.ticket, 
                            request->example1, request->example2);
#else
    EXP_LOG("<<< TEST1: my_nid=%d example1(from)=%d "
                        "example2=%d (good) >>>\n", 
                        my_nid, request->example1, request->example2);
#endif
    /* extra examples */
    // sync
    //down_read(&mm_data->kernel_set_sem);

    // new work (this is done by abs layer!!!!!)
    //INIT_WORK( (struct work_struct*)request_work, process_count_request);
    //queue_work(exit_wq, (struct work_struct*) request_work);

    /* exit() */
    return;
    // if you wanna do pong, plz remember you have to have another 
    // struct remote_thread_first_test_response_t
}

static int test1(void)
{
    int i;
    /* ----- 1st testing ----- */
    /* pre-register corresponding handlers
    // register callback. also define in <linux/pcn_kmsg.h> 
    pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_FIRST_TEST,    // ping - 
                                (pcn_kmsg_cbftn)handle_remote_thread_first_test_request);
    //pcn_kmsg_register_callback(PCN_KMSG_TYPE_FIRST_TEST_HANDLER,              // pong - usually a pair but just simply test here
    //                            handle_remote_thread_first_test_response);
    */

    // compose msg - define -> alloc -> essential msg header info
    remote_thread_first_test_request_t* request; // youTODO: make your own struct 
    request = kmalloc(sizeof(*request), GFP_KERNEL);
    if(request==NULL)
        return -1;

    request->hdr.type = PCN_KMSG_TYPE_FIRST_TEST;   // idx 0 // this indicates if it's a rdma read/write request as well
    request->hdr.prio = PCN_KMSG_PRIO_NORMAL;       // not supported yet
    //request->tgroup_home_cpu = tgroup_home_cpu;   // legacy
    //request->tgroup_home_id = tgroup_home_id;     // legacy

    /* msg essentials */
    /* ------------------------------------------------------------ */
    /* msg dependences */
    request->example1 = my_nid;        // doesn't solve the problem
    request->example2 = 228;
    memset(request->msg,'J', sizeof(request->msg));
    EXP_LOG("\n%s(): testing msg size = strlen(request->msg) %d "
                                        "to all others\n", __func__,
                                            (int)strlen(request->msg));

    // send msg - broadcast // Jack TODO: compared with list_for_each_safe, which one is faster
    for(i=0; i<MAX_NUM_NODES; i++) {
        if(my_nid==i)
            continue;
        //DEBUG_LOG_V("Jack: sending a msg to dst %d ......\n", i);
        pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) request,
                                sizeof(*request));
    }

    //3/13 free manually since user can reuse their buf
    kfree(request); //TODO: uncomment it but will crash!!!
    //DEBUG_LOG_V("Jack's testing DONE! If see N-1 (good), "
    //            "muli-node msg_layer over ipoib is healthy!!!\n");
    return 0;
}

#ifdef CONFIG_POPCORN_KMSG_IB
/* r_read test */
static int test2(void)
{
    volatile int i;
    /* ----- 2nd testing: r_read ----- */
    //////////////////////rdma read//////////////////////////////////
    /*  [compose]
     *  send       ---->   irq (recv)
     *                      perform READ
     * irq (recv)  <-----   send
     * 
     */
    
    // compose msg - define -> alloc -> essential msg header info
    remote_thread_rdma_read_request_t* request_rdma_read; // youTODO: make your own struct 
    request_rdma_read = kmalloc(sizeof(*request_rdma_read), GFP_KERNEL);
    if(request_rdma_read==NULL)
        return -1;
    
    /* (TEST) wrtie data to local buf for verification (THIS SHOULD BE DONE IN PCN_KMSG_SEND_RDMA) */

    request_rdma_read->hdr.type = PCN_KMSG_TYPE_RDMA_READ_REQUEST;  // this indicates if it's a rdma read/write request as well
    request_rdma_read->hdr.prio = PCN_KMSG_PRIO_NORMAL;             // not supported yet
    //request_rdma_read->tgroup_home_cpu = tgroup_home_cpu;
    //request_rdma_read->tgroup_home_id = tgroup_home_id;

    /* msg essentials */
    /* ------------------------------------------------------------ */
    /* msg dependences */

    // compse payload and set the length
    // compose msg - define -> alloc -> essential msg header info
    // just a signal, no msg needed

    /* READ/WRITE specific: *buf, size */
    request_rdma_read->hdr.your_buf_ptr = g_test_buf;       // your buf will be copied to rdma buf for a passive remote read
                                                            // user should protect
    request_rdma_read->hdr.rdma_size = g_test_remote_len;   // size you wanna passive remote to read

    // send msg - broadcast
    for(i=0; i<MAX_NUM_NODES; i++) {
        if(my_nid==i)
            continue;
        
        /* [ib client] sending read key to remote server. this func will take care of EVERYTHING*/
        pcn_kmsg_send_rdma(i, (struct pcn_kmsg_rdma_message*)request_rdma_read, //pcn_kmsg_send_rdma()
                               g_test_remote_len); //1024*4*4
                                                        //TODO kill this arg
        //TODO: make it as a func after WRITE is done according to request_type
        DEBUG_LOG_V("\n\n\n"); 
    }
    kfree(request_rdma_read);
    //DEBUG_LOG_V("Jack's READ testing DONE! If see N-1 (good), muli-node msg_layer over ipoib is healthy!!!\n");
    /////////////////////////rdma read//////////////////////////////////
    return 0;
}

/* r_write test */
static int test3(void)
{
    int i;
    /* ----- 3rd testing: r_write ----- */
    /////////////////////////////rdma write//////////////////////////////
    /* pre-register corresponding handlers
    // register callback. also define in <linux/pcn_kmsg.h> 
    pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_READ_REQUEST,     // ping - 
                                (pcn_kmsg_cbftn)handle_remote_thread_rdma_write_request);
    //pcn_kmsg_register_callback(PCN_KMSG_TYPE_RDMA_READ_REQPONSE,  // pong - usually a pair but just simply test here
    //                            handle_remote_thread_rdma_write_response);
    */

    // compose msg - define -> alloc -> essential msg header info
    remote_thread_rdma_write_request_t* request_rdma_write; // youTODO: make your own struct 
    request_rdma_write = (remote_thread_rdma_write_request_t*) kmalloc(sizeof(remote_thread_rdma_write_request_t), GFP_KERNEL);
    if(request_rdma_write==NULL)
        return -1;

    request_rdma_write->hdr.type = PCN_KMSG_TYPE_RDMA_WRITE_REQUEST;  // this indicates a rdma read/write request
    request_rdma_write->hdr.prio = PCN_KMSG_PRIO_NORMAL;     // not supported yet
    //request_rdma_write->tgroup_home_cpu = tgroup_home_cpu;
    //request_rdma_write->tgroup_home_id = tgroup_home_id;

    /* msg essentials */
    /* ------------------------------------------------------------ */
    /* msg dependences */

    // only length
    // rdma() 
    
    /* READ/WRITE specific: size */
    request_rdma_write->hdr.rdma_size = g_test_remote_len;   // size you wanna passive remote to WRITE
    
    // send msg - broadcast // Jack TODO: compared with list_for_each_safe, which one is faster
    for(i=0; i<MAX_NUM_NODES; i++) {
        if(my_nid==i)
            continue;
        
        // TODO: rdma send
        /* [ib client] sending write key to remote server */
        int rdma_write_len = 4096*4; // size wanna read/write
        pcn_kmsg_send_rdma(i, (struct pcn_kmsg_rdma_message*) request_rdma_write, //pcn_kmsg_send_rdma()
                           rdma_write_len);
        
        /* Wait for server to ACK */   
        /* TODO: use other way to sync e.g.  
        wait_event_interruptible(cb[i]->sem, cb[i]->state >= RDMA_WRITE_ADV);
        if (cb[i]->state != RDMA_WRITE_ADV) {
            printk(KERN_ERR "wait for RDMA_WRITE_ADV state %d\n", cb[i]->state);
            break;  // break
        }
        */
        EXP_LOG("\n\n\n"); 
    }
    kfree(request_rdma_write);
    //DEBUG_LOG_V("Jack's testing DONE! If see N-1 (good), muli-node msg_layer over ipoib is healthy!!!\n");
    /////////////////////rdma write////////////////////////
    return 0;
}
#endif

//static void kthread_test1(void* arg0)
static int kthread_test1(void* arg0)
{
    //struct krping_cb *listening_cb = arg0;
    volatile int i;
    DEBUG_LOG_V("%s(): created\n", __func__);
    for(i=0; i<1000; i++)
        test1(); 
    return 0;
}

#ifdef CONFIG_POPCORN_KMSG_IB
static int kthread_test2(void* arg0)
{
    //struct krping_cb *listening_cb = arg0;
    volatile int i;
    DEBUG_LOG_V("%s(): created\n", __func__);
    for(i=0; i<300; i++)
        test2();   
    return 0;
}

static int kthread_test3(void* arg0)
{
    //struct krping_cb *listening_cb = arg0;
    volatile int i;
    DEBUG_LOG_V("%s(): created\n", __func__);
    for(i=0; i<300; i++)
        test3();   
    return 0;
}

/* testing utility */
int setup_read_buf(void)
{
    volatile int i;
    // read specific (data you wanna let remote side read)                  
    g_test_buf = kmalloc(g_test_remote_len, GFP_KERNEL);                     
    if (!g_test_buf) {                                                      
        printk(KERN_ERR "kmalloc failure\n");                               
        return -ENOMEM;                                                     
    }                                                                       
    memset(g_test_buf, 'R', g_test_remote_len); // mimic: user data buffer ( will be copied to rdma buf)
                                                                            
    /////////////TODO: put it to runtime code and TODO: put lock ///////////
    for(i=0; i<MAX_NUM_NODES; i++) {                                                            
        if(i==my_nid)                                                       
            continue; // canot write to its rdma_buf since addr space
        memcpy(cb[i]->rdma_buf, g_test_buf, g_test_remote_len); //TODO put it to rdma_send and protected by lock
                                                // for active. shoulbe be changed dynamically in runtime TODO: don't hardcoded
    }                                           // TODO: help user to copy their data on a correct buffer window
    ////////////////////////for read/////////////////////////////////////
    smp_mb(); // just in case 
    return 0;
}
#endif

static ssize_t write_proc(struct file * file, 
                    const char __user * buffer, size_t count, loff_t *ppos)
{
    volatile int i;
    char *cmd;
    volatile int cnt=0;
    struct task_struct *t;

    if (!try_module_get(THIS_MODULE)) // 1->2
        return -ENODEV;

    cmd = kmalloc(count, GFP_KERNEL);
    if (cmd == NULL) {
        printk(KERN_ERR "kmalloc failure\n");
        return -ENOMEM;
    }
    if (copy_from_user(cmd, buffer, count)) {
        return -EFAULT;
    }

    // remove the \n.
    cmd[count - 1] = 0;
    printk(KERN_INFO "\n\n[ proc write |%s| cnt %ld ]\n", cmd, count);
    
    /* parse and then do the coresponding function */
    if(cmd[0]=='0') {
        //Reset log
        for (i=0; i<MAX_NUM_NODES; i++)
            ;
            //atomic_set(&cb[i]->stats.send_msgs, 0);
    }
    else if(cmd[0]=='1') {
        while(++cnt<=5000)
            test1();
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
#ifdef CONFIG_POPCORN_KMSG_IB
    else if(cmd[0]=='2') {
        setup_read_buf();
        while(++cnt<=5000)
            test2();
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
    else if(cmd[0]=='3') {
        while(++cnt<=5000)
            test3();
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
#endif
    else if(cmd[0]=='4') { // conccurent multithreading test1()
        for(i=0; i<10; i++) {
            t = kthread_run(kthread_test1, NULL, "kthread_test1()");
            BUG_ON(IS_ERR(t));
        }
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
#ifdef CONFIG_POPCORN_KMSG_IB
    else if(cmd[0]=='5') { // conccurent multithreading test2()
        setup_read_buf();
        for(i=0; i<10; i++) {
            t = kthread_run(kthread_test2, NULL, "kthread_test2()");
            BUG_ON(IS_ERR(t));
        }
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
    else if(cmd[0]=='6') { // conccurent multithreading test3()
        for(i=0; i<10; i++) {
            t = kthread_run(kthread_test3, NULL, "kthread_test3()");
            BUG_ON(IS_ERR(t));
        }
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
    else if(cmd[0]=='9') { // conccurent multithreading test1&2&3() at the same time
        for(i=0; i<10; i++) {
            t = kthread_run(kthread_test1, NULL, "kthread_test1()");
            BUG_ON(IS_ERR(t));
            t = kthread_run(kthread_test2, NULL, "kthread_test2()");
            BUG_ON(IS_ERR(t));
            t = kthread_run(kthread_test3, NULL, "kthread_test3()");
            BUG_ON(IS_ERR(t));
        }
        DEBUG_LOG_V("test%c(done)\n\n\n\n", cmd[0]);
    }
#endif
    else { 
        printk("Not support yet. Try \"1,2,3,4,5,6,9\"\n"); 
    }
 
    kfree(cmd);
    module_put(THIS_MODULE);    // 2->1
    printk("proc write done!!\n");
    return 2;                   // 1->0
}

static int kmsg_test_read_proc(struct seq_file *seq, void *v)
{
    printk("Not suppor open/read\n");
    return 0;
}

static int kmsg_test_read_open(struct inode *inode, struct file *file)
{
        return single_open(file, kmsg_test_read_proc, inode->i_private);
}

static struct file_operations kmsg_test_ops = {
    .owner = THIS_MODULE,
    .open = kmsg_test_read_open,
    .read = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
    .write = write_proc,
};


/* example - main usage */
static int __init msg_layer_test_init(void)
{
    static struct proc_dir_entry *kmsg_test_proc;
       
    /* register a proc */
    printk("\n\n--- Popcorn messaging self testing proc init ---\n");
    kmsg_test_proc = proc_create("kmsg_test", 0666, NULL, &kmsg_test_ops);
    if (kmsg_test_proc == NULL) {
        printk(KERN_ERR "cannot create /proc/kmsg_test\n");
        return -ENOMEM;
    }
    
    /* register callback. also define in <linux/pcn_kmsg.h>  */
    pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_FIRST_TEST,    // ping - 
                    (pcn_kmsg_cbftn)handle_remote_thread_first_test_request);
    //pcn_kmsg_register_callback(PCN_KMSG_TYPE_FIRST_TEST_RESPONSE,              // pong - usually a pair but just simply test here
    //                            handle_remote_thread_first_test_response);
    
    smp_mb(); // Just in case
    printk("--- Popcorn messaging self testing proc init done ---\n");
    printk("--- Usage: sudo echo 1/2/3 > /proc/kmsg_test ---\n");
    printk("---      1: continuously send/recv test ---\n");
    printk("---      2: continuously READ test ---\n");
    printk("---      3: continuously WRITE test ---\n");
    printk("---      4: continuously multithreading read/recv test ---\n");
    printk("---      5: continuously multithreading READ test ---\n");
    printk("---      6: continuously multithreading WRITE test ---\n");
    printk("---      9: continuously multithreading 1+2+3 test ---\n"
                                                        "\n\n\n\n\n\n\n\n");

    /* free */
    return 0;    
}

static void __exit msg_layer_test_unload(void) {
    printk("\n\n--- Popcorn messaging self testing unloaded! ---\n\n");
    remove_proc_entry("kmsg_test", NULL);
}

module_init(msg_layer_test_init);
module_exit(msg_layer_test_unload);
MODULE_LICENSE("GPL");  
