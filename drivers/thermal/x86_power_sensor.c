/* Based on the ARMv8 version written by Sharath Bath and Antonio Barbalace */

//this code doesn't support multipackage systems

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>

#include <asm/msr.h>

const int sampling_interval_ms = 200;
static struct completion start;
static atomic_t stop_sampling;
static atomic_t power_value;
static struct task_struct *pwr_sensor_thread = NULL;
//static struct device *ps_dev = NULL;

int free_running = 1;

#define POPCORN_POWER_N_VALUES 10
extern int *popcorn_power_x86_1;
extern int *popcorn_power_x86_2;

//move the following in a header file (this should be rdtsc)

#define NR_RAPL_DOMAINS 4

static const char * const rapl_domain_names[NR_RAPL_DOMAINS] = {
	"pp0-core",
	"package",
	"dram",
	"pp1-gpu",
};

static int rapl_hw_unit[NR_RAPL_DOMAINS]; /* 1/2^hw_unit Joule */

//from arch/x86/kernel/cpu/perf_event_intel_rapl.c:rapl_check_unit()
static int rapl_init_hw_unit (void)
{
	u64 msr_rapl_power_unit_bits;
	int i;

	if (rdmsrl_safe(MSR_RAPL_POWER_UNIT, &msr_rapl_power_unit_bits))
		return -ENOENT;

	for (i = 0; i < NR_RAPL_DOMAINS; ++i)
		rapl_hw_unit[i] = (msr_rapl_power_unit_bits >> 8) & 0x1FULL;

	return 0;
}

static u64 rapl_scale(int id, u64 data)
{
	// TODO all the value are the same now

	return data * (1000000000UL / (1UL << rapl_hw_unit[0]));
}

unsigned long pp0_tprev = 0;
unsigned long pkg_tprev = 0;
unsigned long dram_tprev = 0;
u64 pp0_prev = 0;
u64 pkg_prev = 0;
u64 dram_prev = 0;

int read_inst_power(int id, u64* prev, unsigned long *tprev)
{
	int i;
	u64 data;
	unsigned long pout;
	long stats; //, lstats, pstats,
	long delta =1;
	long sstart, sstop; //, lstart =0, lstop =0, pstart =0, pstop =0;

	sstart = ktime_to_ns(ktime_get());

	rdmsrl(id, data);

	sstop = ktime_to_ns(ktime_get());

	stats = sstop - sstart;
// lstats = lstop - lstart; pstats = pstop - pstart;

	if (*tprev != 0)
		delta = sstart - *tprev;
	*tprev = sstart;

	if ((long)data < 0) {
		printk(KERN_ERR "%s: MSR read failed [%ld]\n",
				__func__, (long)data);
		pout = -1;
	} else {
		/* Calculate power */

		pout = rapl_scale(id, data - *prev);
		*prev = data;
	}

#if 0
	printk(KERN_ALERT "%s: ktime %ld "
			//              "(hp v %ldms p %ldms) "
			"sensor %lx "
			"en %ldnJ time %ldns "
			"pwr %ldmW\n",
			//		"[%d, %d, %d, %d] \n",
			__func__,
			stats,
			//              (lstats *1000) /freq, (pstats *1000) /freq,
			(unsigned long) data,
			pout, delta,
			((pout * 1000) / delta)
			//                rapl_hw_unit[0], rapl_hw_unit[1], rapl_hw_unit[2], rapl_hw_unit[3]
		  );
#endif

	switch (id) {
	case MSR_PP0_ENERGY_STATUS:
		/* Only keep the POPCORN_POWER_N_VALUES last values */
		for (i = 0; i < POPCORN_POWER_N_VALUES - 1; i++) // TODO use a circular buffer
			popcorn_power_x86_1[i] = popcorn_power_x86_1[i + 1];

		popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1] = (pout * 1000) / delta;
		break;

	case MSR_PKG_ENERGY_STATUS:
		/* Only keep the POPCORN_POWER_N_VALUES last values */
		for (i = 0; i < POPCORN_POWER_N_VALUES - 1; i++)
			popcorn_power_x86_2[i] = popcorn_power_x86_2[i + 1];

		popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1] = (pout * 1000) / delta;
		break;
	}

	return pout;
}

int ps_read_thread(void *arg)
{
	//        int ret = -1;
	int count = 0;
	int local_power = 0;
	int pout = 0;

	printk("%s: Starting...\n", __func__);
	do {
		//printk("%s: Waiting...\n", __func__);
		/* wait for the application to trigger start */
		if (!free_running)
			wait_for_completion(&start);
		//printk("%s: Wokeup...\n", __func__);

		if(kthread_should_stop()) break;

		while(atomic_read(&stop_sampling) == 0) {
			pout = read_inst_power(MSR_PP0_ENERGY_STATUS, &pp0_prev, &pp0_tprev);
			pout = read_inst_power(MSR_PKG_ENERGY_STATUS, &pkg_prev, &pkg_tprev);

			// pout = read_inst_power(MSR_DRAM_ENERGY_STATUS, &dram_prev, &dram_tprev); // this is always ZERO 
			// pout = read_inst_power(MSR_PKG_ENERGY_STATUS, 0, 0); //current processor don't have this power domain

			/*
			if(pout <= 0) {
				printk(KERN_ERR "%s: rdmsr read failed [%d]\n", __func__, ret);
			} else {
			// Accumulate the value
				local_power += pout;
				count++;
				//printk(KERN_ERR "%s: current reading %d\n", __func__, pout);
			}
			*/
			msleep(sampling_interval_ms);
		}

		if (count) {
			atomic_set(&power_value, local_power/count);
		}
	} while(1);
	printk("%s: Stopping...\n", __func__);
	return 0;
}

static int __init pwr_sensor_init(void)
{
	int i;
	int ret = 0;

	printk(KERN_INFO"ps_sensor: start initializing...\n");

	if (rapl_init_hw_unit()) {
		printk(KERN_ERR"ps_sensor: error on rapl_hw_unit initialization\n");
		return -1;
	}

	/* Create kernel thread for periodic reading */
	pwr_sensor_thread = kthread_run(&ps_read_thread, NULL, "Power Sensor Read Thread");

	if(!pwr_sensor_thread) {
		printk(KERN_ERR"ps_sensor: Failed to create kthread\n");
		ret = -1;
	}

	for (i = 0; i < POPCORN_POWER_N_VALUES; i++) {
		popcorn_power_x86_1[i] = 0;
		popcorn_power_x86_2[i] = 0;
	}

	/* Initialize completion variables */
	init_completion(&start);
	atomic_set(&stop_sampling, 0);
	atomic_set(&power_value, 0);
	printk(KERN_INFO"ps_sensor: Initializing finished\n");
	return ret;
}
//module_init(pwr_sensor_init);
late_initcall(pwr_sensor_init);

static void __exit pwr_sensor_exit(void)
{
	printk(KERN_INFO"ps_sensor: Exiting...\n");
	printk(KERN_INFO"ps_sensor: signaling the read thread to exit\n");
	kthread_stop(pwr_sensor_thread);

	/* if free running we are not using the completion */
	if (!free_running) {
		printk(KERN_ALERT"%s: now waiting for completion", __func__);
		complete(&start);
		printk(KERN_ALERT"%s: done waiting for completion", __func__);
	}

	atomic_set(&stop_sampling, 1);

	kthread_stop(pwr_sensor_thread);
	printk(KERN_INFO"ps_sensor: Exited\n");
}
module_exit(pwr_sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antonio Barbalace");
MODULE_DESCRIPTION("Power sensor driver (x86)");
