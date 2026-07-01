#include <stddef.h>
#include <stdint.h>

#include "libfdt.h"

#define DEFAULT_FIT_BASE	0x80000000UL
#define FIT_SEARCH_LIMIT	0x4000UL
#define FIT_SEARCH_STEP		4UL
#define DEFAULT_PAYLOAD_LOAD	0x4a240000UL
#define DEFAULT_PAYLOAD_ENTRY	0x4a240000UL
#define KERNEL_NODE		"kernel-1"
#define DEFAULT_PAYLOAD_NODE	"uboot-1"

extern char _start[];
extern void *memmove(void *dest, const void *src, size_t n);
extern int memcmp(const void *a, const void *b, size_t n);
extern void chainloader_jump(uint64_t entry, uint64_t arg0);

static __attribute__((noreturn)) void hang(void)
{
	for (;;) {
		__asm__ volatile("wfi");
	}
}

static uint32_t fdt_read_u32_default(const void *fdt, int node,
				     const char *prop, uint32_t def)
{
	const fdt32_t *cell;
	int len;

	cell = fdt_getprop(fdt, node, prop, &len);
	if (!cell || len < (int)sizeof(*cell))
		return def;

	return fdt32_to_cpu(*cell);
}

static const char *fdt_read_str_default(const void *fdt, int node,
					const char *prop, const char *def)
{
	const char *str;
	int len;

	str = fdt_getprop(fdt, node, prop, &len);
	if (!str || len <= 1)
		return def;

	return str;
}

static void read_control(const void *control_fdt, uintptr_t *fit_base,
			 const char **payload_node, uintptr_t *fallback_load,
			 uintptr_t *fallback_entry)
{
	int root;

	*fit_base = DEFAULT_FIT_BASE;
	*payload_node = DEFAULT_PAYLOAD_NODE;
	*fallback_load = DEFAULT_PAYLOAD_LOAD;
	*fallback_entry = DEFAULT_PAYLOAD_ENTRY;

	if (!control_fdt || fdt_check_header(control_fdt) != 0)
		return;

	root = fdt_path_offset(control_fdt, "/");
	if (root < 0)
		return;

	*fit_base = fdt_read_u32_default(control_fdt, root, "fit-base",
					 DEFAULT_FIT_BASE);
	*payload_node = fdt_read_str_default(control_fdt, root, "payload-node",
					     DEFAULT_PAYLOAD_NODE);
	*fallback_load = fdt_read_u32_default(control_fdt, root,
					      "fallback-payload-load",
					      DEFAULT_PAYLOAD_LOAD);
	*fallback_entry = fdt_read_u32_default(control_fdt, root,
					       "fallback-payload-entry",
					       DEFAULT_PAYLOAD_ENTRY);
}

static void cache_sync_range(uintptr_t start, size_t len)
{
	uint64_t ctr;
	uintptr_t dline;
	uintptr_t iline;
	uintptr_t from;
	uintptr_t to;

	if (!len)
		return;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

	dline = 4UL << ((ctr >> 16) & 0xf);
	iline = 4UL << (ctr & 0xf);

	from = start & ~(dline - 1);
	to = (start + len + dline - 1) & ~(dline - 1);
	while (from < to) {
		__asm__ volatile("dc civac, %0" : : "r"(from) : "memory");
		from += dline;
	}
	__asm__ volatile("dsb ish" : : : "memory");

	from = start & ~(iline - 1);
	to = (start + len + iline - 1) & ~(iline - 1);
	while (from < to) {
		__asm__ volatile("ic ivau, %0" : : "r"(from) : "memory");
		from += iline;
	}
	__asm__ volatile("dsb ish" : : : "memory");
	__asm__ volatile("isb" : : : "memory");
}

static const void *find_fit_base(uintptr_t start)
{
	uintptr_t addr;

	for (addr = start; addr < start + FIT_SEARCH_LIMIT;
	     addr += FIT_SEARCH_STEP) {
		const void *fit = (const void *)addr;

		if (fdt_check_header(fit) != 0)
			continue;
		if (fdt_path_offset(fit, "/images") < 0)
			continue;

		return fit;
	}

	return 0;
}

static int fit_contains_current_shim(const void *fit)
{
	const void *shim_data;
	int images;
	int kernel_node;
	int len;

	images = fdt_path_offset(fit, "/images");
	if (images < 0)
		return 0;

	kernel_node = fdt_subnode_offset(fit, images, KERNEL_NODE);
	if (kernel_node < 0)
		return 0;

	shim_data = fdt_getprop(fit, kernel_node, "data", &len);
	if (!shim_data || len <= 0)
		return 0;

	return memcmp(shim_data, _start, (size_t)len) == 0;
}

static const void *find_fit_with_fallbacks(uintptr_t primary)
{
	const uintptr_t candidates[] = {
		primary,
		DEFAULT_FIT_BASE,
	};
	const void *first_valid = 0;
	const void *fit;
	size_t i;
	size_t j;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		for (j = 0; j < i; ++j) {
			if (candidates[j] == candidates[i])
				break;
		}
		if (j != i)
			continue;

		fit = find_fit_base(candidates[i]);
		if (!fit)
			continue;

		if (!first_valid)
			first_valid = fit;

		if (fit_contains_current_shim(fit))
			return fit;
	}

	return first_valid;
}

static void disable_mmu_and_caches(void)
{
	uint64_t current_el;
	uint64_t sctlr;
	const uint64_t clear = (1UL << 0) | (1UL << 2) | (1UL << 12);

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
	current_el = (current_el >> 2) & 0x3;

	switch (current_el) {
	case 3:
		__asm__ volatile("mrs %0, sctlr_el3" : "=r"(sctlr));
		sctlr &= ~clear;
		__asm__ volatile("msr sctlr_el3, %0" : : "r"(sctlr) : "memory");
		break;
	case 2:
		__asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
		sctlr &= ~clear;
		__asm__ volatile("msr sctlr_el2, %0" : : "r"(sctlr) : "memory");
		break;
	default:
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
		sctlr &= ~clear;
		__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
		break;
	}

	__asm__ volatile("isb" : : : "memory");
}

void chainloader_main(const void *control_fdt)
{
	const void *fit;
	const void *payload_data;
	const char *payload_node_name;
	uintptr_t fit_base;
	uintptr_t payload_load;
	uintptr_t payload_entry;
	size_t payload_len;
	int images;
	int payload_node;
	int len;
	const fdt32_t *loadp;
	const fdt32_t *entryp;

	read_control(control_fdt, &fit_base, &payload_node_name,
		     &payload_load, &payload_entry);

	fit = find_fit_with_fallbacks(fit_base);
	if (!fit)
		hang();

	images = fdt_path_offset(fit, "/images");
	if (images < 0)
		hang();

	payload_node = fdt_subnode_offset(fit, images, payload_node_name);
	if (payload_node < 0)
		hang();

	payload_data = fdt_getprop(fit, payload_node, "data", &len);
	if (!payload_data || len <= 0)
		hang();
	payload_len = (size_t)len;

	loadp = fdt_getprop(fit, payload_node, "load", &len);
	if (loadp && len >= (int)sizeof(*loadp))
		payload_load = fdt32_to_cpu(*loadp);

	entryp = fdt_getprop(fit, payload_node, "entry", &len);
	if (entryp && len >= (int)sizeof(*entryp))
		payload_entry = fdt32_to_cpu(*entryp);

	memmove((void *)payload_load, payload_data, payload_len);
	cache_sync_range(payload_load, payload_len);
	disable_mmu_and_caches();
	chainloader_jump(payload_entry, 0);
	hang();
}
