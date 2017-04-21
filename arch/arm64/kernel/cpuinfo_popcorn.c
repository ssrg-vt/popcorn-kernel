/**
 * @file cpuinfo_popcorn.c
 *
 * Popcorn Linux ARM64 cpuinfo implementation
 * This work is a rework of Ajithchandra Saya's implementation
 * to provide the ARM64 specific information for remote cpuinfo.
 * The original implementation was based on the custom Linux kernel
 * for the X-Gene, but, the current implementation is based on
 * the vanilla Linux kernel.
 *
 * @author Ajithchandra Saya, SSRG, VirginiaTech 2014
 * @author Jingoo Han, SSRG Virginia Tech 2017
 */

#include <asm/cpu.h>

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/elf.h>
#include <linux/personality.h>

#include <popcorn/cpuinfo.h>
#include <popcorn/pcn_kmsg.h>

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

int fill_cpu_info(_remote_cpu_info_data_t *res)
{
	int i, j;
	bool compat = personality(current->personality) == PER_LINUX32;
	unsigned int count = 0;
	cpuinfo_arch_arm64_t *arch = &res->arch.arm64;

	res->arch_type = arch_arm;

	for_each_online_cpu(i) {
		struct cpuinfo_arm64 *cpuinfo = &per_cpu(cpu_data, i);
		u32 midr = cpuinfo->reg_midr;

#ifdef CONFIG_SMP
		arch->percore[count].processor_id = i;
#endif

		if (compat) {
			arch->percore[count].compat = true;
			strcpy(arch->percore[count].model_name, "ARMv8 Processor");
			arch->percore[count].model_rev = MIDR_REVISION(midr);
			strcpy(arch->percore[count].model_elf, COMPAT_ELF_PLATFORM);
		} else {
			arch->percore[count].compat = false;
			strcpy(arch->percore[count].model_name, "ARMv8 Processor");
		}

		arch->percore[count].bogo_mips = loops_per_jiffy / (500000UL/HZ);
		arch->percore[count].bogo_mips_fraction = loops_per_jiffy / (5000UL/HZ) % 100;

		strcpy(arch->percore[count].flags, "");
		if (compat) {
#ifdef CONFIG_COMPAT
			for (j = 0; compat_hwcap_str[j]; j++) {
				if (compat_elf_hwcap & (1 << j)) {
					strcat(arch->percore[count].flags, compat_hwcap_str[j]);
					strcat(arch->percore[count].flags, " ");
				}
			}

			for (j = 0; compat_hwcap2_str[j]; j++) {
				if (compat_elf_hwcap2 & (1 << j)) {
					strcat(arch->percore[count].flags, compat_hwcap2_str[j]);
					strcat(arch->percore[count].flags, " ");
				}
			}
#endif /* CONFIG_COMPAT */
		} else {
			for (j = 0; hwcap_str[j]; j++) {
				if (elf_hwcap & (1 << j)) {
					strcat(arch->percore[count].flags, hwcap_str[j]);
					strcat(arch->percore[count].flags, " ");
				}
			}
		}

		arch->percore[count].cpu_implementer = MIDR_IMPLEMENTOR(midr);
		arch->percore[count].cpu_archtecture = 8;
		arch->percore[count].cpu_variant = MIDR_VARIANT(midr);
		arch->percore[count].cpu_part = MIDR_PARTNUM(midr);
		arch->percore[count].cpu_revision = MIDR_REVISION(midr);

		count++;
		arch->num_cpus = count;
	}

	return 0;
}
