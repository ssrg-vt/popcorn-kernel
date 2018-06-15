#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "power_sensor.h"
#include "apm_i2c_access.h"

#include "apm_i2c_access.c"

//move the following in a header file
#define HP_TIMING_NOW(var) \
  __asm__ __volatile__ ("isb; mrs %0, cntvct_el0" : "=r" (var) )
#define HP_TIMING_NOW_P(var) \
  __asm__ __volatile__ ("isb; mrs %0, cntpct_el0" : "=r" (var) )

#define HP_TIMING_FREQ(var) \
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r" (var) )


#define DEV_NAME	"Power Sensor"
#define I2C_BUS		IIC_1
#define I2C_SA		0x46
#define I2C_RA		0x96
#define RD_LENGTH	2

static int major = 0;
static struct class *ps_class;
static struct completion start;
static struct completion stop;
static int sampling_interval = 250; // in ms
static atomic_t stop_sampling;
static atomic_t power_value;
static atomic_t exit_ps_read_thread;
static struct task_struct *ps_thread = NULL;
static dev_t ps_dev_num;
static struct device *ps_dev = NULL;

int free_running =1;
// TODO add module param for this

#define POPCORN_POWER_N_VALUES 10
extern int *popcorn_power_arm_1;
extern int *popcorn_power_arm_2;
extern int *popcorn_power_arm_3;

//ktime_t 
unsigned long sstart, sstop;
unsigned long lstart, lstop, pstart, pstop;
unsigned long freq =0;

//code from "Application Note 135"
//available at http://cds.linear.com/docs/en/application-note/an135f.pdf
/* float L11_to_float(u16 input_val)
{
	char exponent = input_val >> 11;
	short mantissa = input_val & 0x7ff;
	
	if (exponent > 0x00F)
		exponent |= 0xE0;
	if (mantissa > 0x3FF)
		mantissa |=0xF800;
	return mantissa * pow(2, exponent);
} */
//the following returns mW
unsigned long L5_L11_to_long(u16 input_val)
{
	char exponent = (input_val >> 11) & 0x1f;
        unsigned long mantissa = input_val & 0x7ff;
        
        if (mantissa > 0x3ff)
                mantissa |= ~0x7ff;
	mantissa *= 1000; 

	if (exponent > 0x00f) {
		exponent |= ~0x00f;
		return mantissa / (1 << (-1 * exponent));
	}
	else
		return mantissa * (1 << exponent);
}

static int apm_regs[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
static int apm_bytes[] = {2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2,
	2, 2, 2, 2, 2, 2};

static int apm_dump_reg(int bus, unsigned char sa, unsigned char offset, int length)
{
	u32 data;
	int ret = -1;
	int pout;
	long sstart, sstop, sstats;

if (length > 4)
	return -1;

	sstart = ktime_to_ns(ktime_get());	
	ret = i2c_sensor_read(bus, sa, offset, length, &data);
	sstop = ktime_to_ns(ktime_get());
	sstats = sstop - sstart;

	if(ret < 0) {
		printk(KERN_ERR "%s: ktime %ld "
				"sa %x offset %x "
				"I2C read failed [%d]\n",
				__func__, 
				sstats,
				(int) sa, (int) offset, ret);
				
		return pout = -1;
	} 

        printk(KERN_ALERT "%s: ktime %ld "
		"sa %x offset %x val %x\n",
                __func__,
		sstats, 
		(int)sa, (int) offset, data);

	return pout = data;
}

#define APM_I2C_SLAVE_ADDR 0x32
/* this is about special I2C register 0x32 that you can access through SLIMpro emulated I2C device */
/* if you put 0x33 all readings fail with -19 */
static void apm_dump_all(void)
{
	int i;
	for (i = 0; i < sizeof(apm_regs)/sizeof(int); i++)
		apm_dump_reg(I2C_BUS, APM_I2C_SLAVE_ADDR, apm_regs[i], apm_bytes[i]);
}	

int read_inst_power(int bus, unsigned char sa, unsigned char offset)
{
	int i;
        u32 data;
        int ret = -1;
        int N, Y, pout;
        long stats, lstats, pstats;

        sstart = ktime_to_ns(ktime_get());
        HP_TIMING_NOW(lstart); HP_TIMING_NOW_P(pstart);
        ret = i2c_sensor_read(bus, sa, offset, RD_LENGTH, &data);

        HP_TIMING_NOW(lstop); HP_TIMING_NOW_P(pstop);
        sstop = ktime_to_ns(ktime_get());
        stats = sstop - sstart;
        lstats = lstop - lstart; pstats = pstop - pstart;
        if(ret < 0) {
                printk(KERN_ERR "%s: I2C read failed %d %d [%d]\n",
                                __func__, (int)bus, (int)sa, ret);
                pout = -1;
        } else {
                /* Calculate power */
                N = 32 - ((data >> 11) & 0x1F);
                Y = data & 0x7ff;
                pout = (int)((Y *2 >> N));
        }
/*        printk(KERN_ALERT "%s: ktime %ld "
//              "(hp v %ldms p %ldms) "
                "sensor %x %x "
                "pwr %x, %d, %d, %ldmW\n",
                __func__,
                stats,
//              (lstats *1000) /freq, (pstats *1000) /freq,
                (int)sa, (int)offset,
                data, (data & 0x7ff), ((data >> 11) & 0x1f), L5_L11_to_long(data));
*/
	switch (sa) {
		case 0x46:
			/* Only keep the POPCORN_POWER_N_VALUES last values */
			for (i = 0; i < POPCORN_POWER_N_VALUES - 1; i++)
				popcorn_power_arm_1[i] = popcorn_power_arm_1[i + 1];

			popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1] = L5_L11_to_long(data);
			break;
		case 0x47:
			/* Only keep the POPCORN_POWER_N_VALUES last values */
			for (i = 0; i < POPCORN_POWER_N_VALUES - 1; i++)
				popcorn_power_arm_2[i] = popcorn_power_arm_2[i + 1];

			popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1] = L5_L11_to_long(data);
			break;
		case 0x40:
			/* Only keep the POPCORN_POWER_N_VALUES last values */
			for (i = 0; i < POPCORN_POWER_N_VALUES - 1; i++)
				popcorn_power_arm_3[i] = popcorn_power_arm_3[i + 1];

			popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1] = L5_L11_to_long(data);
			break;
	}

        return pout;
}

int ps_read_thread(void *arg)
{	
	int ret = -1;
	int count = 0;
	int local_power = 0;
	int pout = 0;

	printk("%s: Starting...\n", __func__);
	do
	{
		//printk("%s: Waiting...\n", __func__);
		/* wait for the application to trigger start */
if (!free_running)
		wait_for_completion(&start);
		//printk("%s: Wokeup...\n", __func__);

		if(kthread_should_stop() || (atomic_read(&exit_ps_read_thread) == 1)) {
			break;
		}

		while(atomic_read(&stop_sampling) == 0)
		{
			//pout = read_inst_power(I2C_BUS, I2C_SA, I2C_RA);
			
			// PMD power = 0x46+0x47 (core)
			// SoC power = 0x40 (uncore)
			read_inst_power(I2C_BUS, 0x46, I2C_RA);
			read_inst_power(I2C_BUS, 0x47, I2C_RA);
                        read_inst_power(I2C_BUS, 0x40, I2C_RA);

//			read_inst_power(I2C_BUS, 0x32, 0x20);
	//		read_inst_power(I2C_BUS, 0x32, 0x21);
/*
			if(pout <= 0) {
				printk(KERN_ERR "%s: I2C read failed [%d]\n",
						__func__, ret);
			} else {
				// Accumulate the value
				local_power += pout;
				count++;
//printk(KERN_ERR "%s: current reading %d\n", __func__, pout);
			}
*/
			msleep(sampling_interval);
		}

		if(count) {
			atomic_set(&power_value, local_power/count);
		}
		complete(&stop);
	} while(1);
	printk("%s: Stopping...\n", __func__);
	return 0;
}

static long ps_unlocked_ioctl(struct file   *file,
		unsigned int  cmd,
		unsigned long data)
{
	int ret = -1;
	int pwr = 0;
	void __user *arg = (void __user *) data;

	//printk("%s+\n", __func__);

	switch (cmd)
	{
		case PSCTL_START:
		{
// TODO we have to decide how to use this interface with free_running actuvated, probably instead of doing the completion just return the last value we read 

			//printk("%s: PSCTL_START\n", __func__);
			/* clear the average power value */
			atomic_set(&stop_sampling, 0);
			atomic_set(&power_value, 0);
			complete(&start);
			ret = 0;
			break;
		}

		case PSCTL_STOP:
		{
//TODO this interface provide a kind of narrow way to stop the system ... should be rewritten

			//printk("%s: PSCTL_STOP\n", __func__);
			atomic_set(&stop_sampling, 1);
			wait_for_completion(&stop);

			/* read and return the average */
			/* power value to user         */
			pwr = atomic_read(&power_value);

			if(pwr != 0) {
				ret = copy_to_user(arg, &pwr, sizeof(int));
			} else {
				ret = -1;
			}
			break;
		}

		case PSCTL_READ_PWR:
		{
			//printk("%s: PSCTL_READ_PWR:\n", __func__, cmd);
			pwr = read_inst_power(I2C_BUS, I2C_SA, I2C_RA);

			if(pwr != 0) {
				ret = copy_to_user(arg, &pwr, sizeof(int));
			} else {
				ret = -1;
			}
			break;
		}

		default:
		{
			//printk("%s: Err: Invalid command - %d\n", __func__, cmd);
			ret = -1;
			break;
		}
	}
	//printk("%s-\n", __func__);
	return ret;
}

static int ps_open(struct inode *inode, struct file *file)
{
	//printk("%s+\n", __func__);
	//printk("%s-\n", __func__);
	return 0;
}

static int ps_release(struct inode *inode, struct file *file)
{
	//printk("%s+\n", __func__);
	//printk("%s-\n", __func__);
	return 0;
}

static const struct file_operations ps_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ps_unlocked_ioctl,
	.open           = ps_open,
	.release        = ps_release,
};

// sa is slave address
static void mfr_model_id (int bus, unsigned char sa, unsigned char offset, int flag)
{
	int ret =0;
        char data[32];
        memset(data, 0, 32);

        ret = i2c_sensor_read_any(bus, sa, offset, 12, (u32*)&data[0]);
	data[31]=0;
        if (ret < 0)
                printk(KERN_ERR"%s: i2c_sensor_read errori\n", __func__);
        else {
		if (flag)
                printk(KERN_ERR"%s: slave addr %x cmd %x string %s\n",
                        __func__, (int) sa, (int) offset, data); 
		else
		printk(KERN_ERR"%s: slave addr %x cmd %x hex %x %x %x\n",
                        __func__, (int) sa, (int) offset,
                        *(unsigned int*)&data[0], *(unsigned int*)&data[4], 
			*(unsigned int*)&data[8]);
	}
}

static int __init pwr_sensor_init(void)
{
	int i;
	int ret = 0;
//	unsigned long freq= 0;

	printk(KERN_ALERT "\nps_sensor: Power sensor driver - Initializing...\n");
/*
	ps_class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(ps_class)) {
		printk(KERN_ERR "pwr_sensor: can't register device class\n");
		ret = -1;
		goto cleanup;
	}

	major = register_chrdev(0, DEV_NAME, &ps_fops);

	if(major < 0) {
		printk(KERN_ERR "pwr_sensor: failed to register device\n");
		ret = -1;
		goto cleanup;
	}

	ps_dev_num = MKDEV(major, 0);

	ps_dev = device_create(ps_class, NULL, ps_dev_num, NULL, "power_sensor0");
	if(!ps_dev) {
		printk(KERN_ERR "pwr_sensor: failed to create dev entry\n");
		ret = -1;
		goto cleanup;
	}
*/
/*
HP_TIMING_FREQ(freq);
printk(KERN_ALERT "timer freq is %lu\n", freq);

	//0x9a is MFR_MODEL that is "LTC3880" string in LTC3880
	//0xe7 is MFR_SPECIAL_ID that is 0x40X for LTC3880
	mfr_model_id(I2C_BUS, I2C_SA, 0x9a, 1); //0x9a);
        mfr_model_id(I2C_BUS, I2C_SA, 0x9e, 0);
	mfr_model_id(I2C_BUS, 0x47, 0x9a, 1); //0x9a);
        mfr_model_id(I2C_BUS, 0x47, 0x9e, 0);
	mfr_model_id(I2C_BUS, 0x40, 0x9a, 1); //0x9a);
        mfr_model_id(I2C_BUS, 0x40, 0x9e, 0);
        mfr_model_id(I2C_BUS, 0x48, 0x9a, 1); //0x9a);
        mfr_model_id(I2C_BUS, 0x48, 0x9e, 0);
*/
/* experimental */
/*	mfr_model_id(I2C_BUS, 0x24, 0x9a, 0);
        mfr_model_id(I2C_BUS, 0x1c, 0x9a, 0);
        mfr_model_id(I2C_BUS, 0x1f, 0x9a, 0);
*/
//apm_dump_all();

	/* Create kernel thread for periodic reading */
	ps_thread = kthread_run(&ps_read_thread, 
			NULL, "Power Sensor Read Thread");

	if(!ps_thread) {
		printk(KERN_ERR "pwr_sensor: Failed to create kthread\n");
		ret = -1;
		goto cleanup;
	}

	for (i = 0; i < POPCORN_POWER_N_VALUES; i++) {
		popcorn_power_arm_1[i] = 0;
		popcorn_power_arm_2[i] = 0;
		popcorn_power_arm_3[i] = 0;
	}

	/* Initialize completion variables */
	init_completion(&start);
	init_completion(&stop);
	atomic_set(&stop_sampling, 0);
	atomic_set(&power_value, 0);
	atomic_set(&exit_ps_read_thread, 0);
	printk(KERN_ALERT "ps_sensor: Power sensor driver - Initializing...[Done]\n");
	return ret;

cleanup:
	printk(KERN_INFO "ps_sensor: Power sensor driver - Initializing...[Failed]\n");
	if(major > 0) {
		printk(KERN_INFO "ps_sensor: unregistering character device\n");
		unregister_chrdev(major, DEV_NAME);
	}

	if(!IS_ERR(ps_class)) {
		if(ps_dev) {
			printk(KERN_INFO "ps_sensor: destroying device created\n");
			device_destroy(ps_class, ps_dev_num);
		}
		printk(KERN_INFO "ps_sensor: destroying class\n");
		class_destroy(ps_class);
	}

	return ret;
}
//module_init(pwr_sensor_init);
late_initcall(pwr_sensor_init);

static void __exit pwr_sensor_exit(void)
{
	printk(KERN_INFO "\nps_sensor: Power sensor driver - Exiting...\n");
	if(major > 0) {
		printk(KERN_INFO "ps_sensor: unregistering character device\n");
		unregister_chrdev(major, DEV_NAME);
	}

	if(!IS_ERR(ps_class)) {
		if(ps_dev) {
			printk(KERN_INFO "ps_sensor: destroying device created\n");
			device_destroy(ps_class, ps_dev_num);
		}
		printk(KERN_INFO "ps_sensor: destroying class\n");
		class_destroy(ps_class);
	}

	printk(KERN_INFO "ps_sensor: signaling the read thread to exit\n");
	atomic_set(&exit_ps_read_thread, 1);

/* if free running we are not using the completion */
if (!free_running) {
printk(KERN_ALERT "%s: now waiting for completion", __func__);
	complete(&start);
printk(KERN_ALERT "%s: done waiting for completion", __func__);
}

        atomic_set(&stop_sampling, 1);


	kthread_stop(ps_thread);
	printk(KERN_INFO "ps_sensor: Power sensor driver - Exiting...[Done]\n");
}
module_exit(pwr_sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sharath Kumar Bhat");
MODULE_AUTHOR("Antonio Barbalace");
MODULE_DESCRIPTION("Power sensor driver");

