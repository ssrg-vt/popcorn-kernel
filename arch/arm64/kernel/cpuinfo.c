/*
 * Record and handle CPU attributes.
 *
 * Copyright (C) 2014 ARM Ltd.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/bitops.h>
#include <linux/elf.h>
#include <linux/kernel.h>
#include <linux/personality.h>
#include <linux/seq_file.h>
#include <linux/delay.h>

#include <asm/cpu.h>

#include <popcorn/cpuinfo.h>

static struct cpu_global_info {
	unsigned int remote;
	unsigned int vpos;
	unsigned int nid;
} cpu_global_info;

/*
 * num_cpus: # of cores of each nodes
 * num_total_cpus: # of total cpus of all connected nodes
 */
static unsigned int num_cpus[MAX_POPCORN_NODES];
static unsigned int num_total_cpus;

/*
 * In case the boot CPU is hotpluggable, we record its initial state and
 * current state separately. Certain system registers may contain different
 * values depending on configuration at or after reset.
 */
DEFINE_PER_CPU(struct cpuinfo_arm64, cpu_data);
static struct cpuinfo_arm64 boot_cpu_data;

static char *icache_policy_str[] = {
	[ICACHE_POLICY_RESERVED] = "RESERVED/UNKNOWN",
	[ICACHE_POLICY_AIVIVT] = "AIVIVT",
	[ICACHE_POLICY_VIPT] = "VIPT",
	[ICACHE_POLICY_PIPT] = "PIPT",
};

unsigned long __icache_flags;

static const char *const hwcap_str[] = {
	"fp",
	"asimd",
	"evtstrm",
	"aes",
	"pmull",
	"sha1",
	"sha2",
	"crc32",
	"atomics",
	NULL
};

#ifdef CONFIG_COMPAT
static const char *const compat_hwcap_str[] = {
	"swp",
	"half",
	"thumb",
	"26bit",
	"fastmult",
	"fpa",
	"vfp",
	"edsp",
	"java",
	"iwmmxt",
	"crunch",
	"thumbee",
	"neon",
	"vfpv3",
	"vfpv3d16",
	"tls",
	"vfpv4",
	"idiva",
	"idivt",
	"vfpd32",
	"lpae",
	"evtstrm",
	NULL
};

static const char *const compat_hwcap2_str[] = {
	"aes",
	"pmull",
	"sha1",
	"sha2",
	"crc32",
	NULL
};
#endif /* CONFIG_COMPAT */

static int c_show_arm64(struct seq_file *m, void *v)
{
	int i, j;
	bool compat = personality(current->personality) == PER_LINUX32;

	for_each_online_cpu(i) {
		struct cpuinfo_arm64 *cpuinfo = &per_cpu(cpu_data, i);
		u32 midr = cpuinfo->reg_midr;

		/*
		 * glibc reads /proc/cpuinfo to determine the number of
		 * online processors, looking for lines beginning with
		 * "processor".  Give glibc what it expects.
		 */
		seq_printf(m, "processor\t: %d\n", i);
		if (compat)
			seq_printf(m, "model name\t: ARMv8 Processor rev %d (%s)\n",
				   MIDR_REVISION(midr), COMPAT_ELF_PLATFORM);

		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
			   loops_per_jiffy / (500000UL/HZ),
			   loops_per_jiffy / (5000UL/HZ) % 100);

		/*
		 * Dump out the common processor features in a single line.
		 * Userspace should read the hwcaps with getauxval(AT_HWCAP)
		 * rather than attempting to parse this, but there's a body of
		 * software which does already (at least for 32-bit).
		 */
		seq_puts(m, "Features\t:");
		if (compat) {
#ifdef CONFIG_COMPAT
			for (j = 0; compat_hwcap_str[j]; j++)
				if (compat_elf_hwcap & (1 << j))
					seq_printf(m, " %s", compat_hwcap_str[j]);

			for (j = 0; compat_hwcap2_str[j]; j++)
				if (compat_elf_hwcap2 & (1 << j))
					seq_printf(m, " %s", compat_hwcap2_str[j]);
#endif /* CONFIG_COMPAT */
		} else {
			for (j = 0; hwcap_str[j]; j++)
				if (elf_hwcap & (1 << j))
					seq_printf(m, " %s", hwcap_str[j]);
		}
		seq_puts(m, "\n");

		seq_printf(m, "CPU implementer\t: 0x%02x\n",
			   MIDR_IMPLEMENTOR(midr));
		seq_printf(m, "CPU architecture: 8\n");
		seq_printf(m, "CPU variant\t: 0x%x\n", MIDR_VARIANT(midr));
		seq_printf(m, "CPU part\t: 0x%03x\n", MIDR_PARTNUM(midr));
		seq_printf(m, "CPU revision\t: %d\n\n", MIDR_REVISION(midr));
	}

	return 0;
}

static void calc_nid_vpos(loff_t *pos, unsigned int *pnid, unsigned int *vpos)
{
	int i = 0;

	*pnid = 0;
	*vpos = 0;

	for (i = 1; i <= num_total_cpus; i++) {
		if ((*pnid) == my_nid)
			(*pnid)++;

		if ((*vpos) == num_cpus[*pnid]) {
			*vpos = 0;
			(*pnid)++;
		}

		if (i == (*pos))
			break;

		(*vpos)++;
	}
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	unsigned int vpos = 0;
	unsigned int nid = 0;

	if (my_nid == -1 || (*pos) == 0)
		goto local;

	if ((*pos) == 1) {
		int i = 0;
		int j = 0;
		bool connected = false;

		/* Check the connection with remote nodes */
		for (i = 0; i < MAX_POPCORN_NODES; i++) {
			if (get_popcorn_node_online(i)) {
				connected = true;
				break;
			}
		}

		if (connected == false) {
			/* No connection */
			goto local;
		} else {
			/* Connection with remote nodes */
			for (i = 0; i < MAX_POPCORN_NODES; i++) {
				if (i == my_nid) {
					num_cpus[i] = 1;
					j = j + 1;
					continue;
				}
				if (get_popcorn_node_online(i)) {
					send_remote_cpu_info_request(i);
					num_cpus[i] = get_number_cpus_from_remote_node(i);
					j = j + num_cpus[i];
				} else {
					num_cpus[i] = 0;
				}
			}

			num_total_cpus = j;
			goto remote;
		}
	} else if ((*pos) > 1) {
		goto remote;
	}
local:
	if ((*pos) < 1) {
		cpu_global_info.remote = 0;

		return &cpu_global_info;
	}

	return NULL;

remote:
	if ((*pos) < num_total_cpus) {
		calc_nid_vpos(pos, &nid, &vpos);

		cpu_global_info.remote = 1;
		cpu_global_info.vpos = vpos;
		cpu_global_info.nid = nid;

		return &cpu_global_info;
	}

	return NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

static int c_show(struct seq_file *m, void *v)
{
	struct cpu_global_info *cpu_global_info = v;

	if (cpu_global_info->remote == 1) {
		remote_proc_cpu_info(m, cpu_global_info->nid, cpu_global_info->vpos);
	} else {
		c_show_arm64(m, v);
	}

	return 0;
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};

static void cpuinfo_detect_icache_policy(struct cpuinfo_arm64 *info)
{
	unsigned int cpu = smp_processor_id();
	u32 l1ip = CTR_L1IP(info->reg_ctr);

	if (l1ip != ICACHE_POLICY_PIPT) {
		/*
		 * VIPT caches are non-aliasing if the VA always equals the PA
		 * in all bit positions that are covered by the index. This is
		 * the case if the size of a way (# of sets * line size) does
		 * not exceed PAGE_SIZE.
		 */
		u32 waysize = icache_get_numsets() * icache_get_linesize();

		if (l1ip != ICACHE_POLICY_VIPT || waysize > PAGE_SIZE)
			set_bit(ICACHEF_ALIASING, &__icache_flags);
	}
	if (l1ip == ICACHE_POLICY_AIVIVT)
		set_bit(ICACHEF_AIVIVT, &__icache_flags);

	pr_info("Detected %s I-cache on CPU%d\n", icache_policy_str[l1ip], cpu);
}

static void __cpuinfo_store_cpu(struct cpuinfo_arm64 *info)
{
	info->reg_cntfrq = arch_timer_get_cntfrq();
	info->reg_ctr = read_cpuid_cachetype();
	info->reg_dczid = read_cpuid(DCZID_EL0);
	info->reg_midr = read_cpuid_id();

	info->reg_id_aa64dfr0 = read_cpuid(ID_AA64DFR0_EL1);
	info->reg_id_aa64dfr1 = read_cpuid(ID_AA64DFR1_EL1);
	info->reg_id_aa64isar0 = read_cpuid(ID_AA64ISAR0_EL1);
	info->reg_id_aa64isar1 = read_cpuid(ID_AA64ISAR1_EL1);
	info->reg_id_aa64mmfr0 = read_cpuid(ID_AA64MMFR0_EL1);
	info->reg_id_aa64mmfr1 = read_cpuid(ID_AA64MMFR1_EL1);
	info->reg_id_aa64pfr0 = read_cpuid(ID_AA64PFR0_EL1);
	info->reg_id_aa64pfr1 = read_cpuid(ID_AA64PFR1_EL1);

	info->reg_id_dfr0 = read_cpuid(ID_DFR0_EL1);
	info->reg_id_isar0 = read_cpuid(ID_ISAR0_EL1);
	info->reg_id_isar1 = read_cpuid(ID_ISAR1_EL1);
	info->reg_id_isar2 = read_cpuid(ID_ISAR2_EL1);
	info->reg_id_isar3 = read_cpuid(ID_ISAR3_EL1);
	info->reg_id_isar4 = read_cpuid(ID_ISAR4_EL1);
	info->reg_id_isar5 = read_cpuid(ID_ISAR5_EL1);
	info->reg_id_mmfr0 = read_cpuid(ID_MMFR0_EL1);
	info->reg_id_mmfr1 = read_cpuid(ID_MMFR1_EL1);
	info->reg_id_mmfr2 = read_cpuid(ID_MMFR2_EL1);
	info->reg_id_mmfr3 = read_cpuid(ID_MMFR3_EL1);
	info->reg_id_pfr0 = read_cpuid(ID_PFR0_EL1);
	info->reg_id_pfr1 = read_cpuid(ID_PFR1_EL1);

	info->reg_mvfr0 = read_cpuid(MVFR0_EL1);
	info->reg_mvfr1 = read_cpuid(MVFR1_EL1);
	info->reg_mvfr2 = read_cpuid(MVFR2_EL1);

	cpuinfo_detect_icache_policy(info);

	check_local_cpu_errata();
}

void cpuinfo_store_cpu(void)
{
	struct cpuinfo_arm64 *info = this_cpu_ptr(&cpu_data);
	__cpuinfo_store_cpu(info);
	update_cpu_features(smp_processor_id(), info, &boot_cpu_data);
}

void __init cpuinfo_store_boot_cpu(void)
{
	struct cpuinfo_arm64 *info = &per_cpu(cpu_data, 0);
	__cpuinfo_store_cpu(info);

	boot_cpu_data = *info;
	init_cpu_features(&boot_cpu_data);
}
