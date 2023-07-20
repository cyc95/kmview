// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "SMP alternatives: " fmt

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/stringify.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/memory.h>
#include <linux/stop_machine.h>
#include <linux/slab.h>
#include <linux/kdebug.h>
#include <linux/kprobes.h>
#include <linux/mmu_context.h>
#include <linux/bsearch.h>
#include <linux/sync_core.h>
#include <linux/kmview.h>
#include <asm/text-patching.h>
#include <asm/alternative.h>
#include <asm/kmview_alternative.h>
#include <asm/sections.h>
#include <asm/mce.h>
#include <asm/nmi.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/insn.h>
#include <asm/io.h>
#include <asm/fixmap.h>
#include <asm/paravirt.h>
#include <asm/asm-prototypes.h>




#define MAX_PATCH_LEN (256*256-2)


static int
kmview_patching_pte_range(pmd_t *pmd, unsigned long addr, const void *opcode, unsigned long end)
{
	pte_t *pte;
	pte_t *ptep
	char *opcode_addr = (char *) opcode;
	unsigned long addrs = addr;
	// Get the whole pte
	pte = (pte_t *)pmd_page_vaddr(*pmd);

	pte += pte_index(addr);

	do {
			struct page *page;
			char *page_addr;
			pte_t ptek;		
			ptep = pte_offset_map(pmd, addr);
			ptek = *ptep;
			BUG_ON(!pte_present(ptek));
			page = pte_page(ptek);
			pte_unmap(ptep);
			page_addr = page_address(page);
			mutex_lock(&text_mutex);
			for(; addrs < addr + PAGE_SIZE; addrs++){
				page_addr[addrs & ~PAGE_MASK] = * opcode_addr; 
				opcode_addr ++;
			}
			mutex_unlock(&text_mutex);

	} while (pte++, addr += PAGE_SIZE, addr < end);

	return 0;
}

static inline int
kmview_patching_pmd_range(pud_t *pud, unsigned long addr, const void *opcode, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long addrs = addr;
	char *opcode_addr = (char *) opcode;
	pmd = pud_pgtable(*pud);

	pmd += pmd_index(addr);

	do {
		next = pmd_addr_end(addr, end);

		if (pmd_none(*pmd))
			continue;

		if (pmd_large(*pmd)) {
			struct page *page;
			char *page_addr;

			page = pmd_page(*pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
			page_addr = page_address(page);
			mutex_lock(&text_mutex);
			for(; addrs < end; addrs++){
				page_addr[addrs & ~PAGE_MASK] = * opcode_addr; 
				opcode_addr ++;
			}
			mutex_unlock(&text_mutex);
		} else {
			int err;
			/* printk(KERN_INFO "C PMD: %lx  (%lx)\n", */
			/*        addr, */
			/*        pmd_val(*dst_pmd)); */
			err = kmview_patching_pmd_range(pmd, addr, opcode + next - addr, next);
			if (err)
				return err;
		}
	} while (pmd++, addr = next, addr < end);

	return 0;
}



static int
kmview_patching_pud_range(pud_t *pud, unsigned long addr, const void *opcode,
				      unsigned long *end)
{
	unsigned long next;
	pud += pud_index(addr);

	do {
		next = pud_addr_end(addr, end);

		if (pud_none(*pud))
			continue;

		if (pud_large(*pud)) {
			/* printk(KERN_INFO "C LARGE PUD: %lx  (%lx)\n", */
			/*        addr, */
			/*        pud_val(*pud)); */
			// TODO copy huge page
			BUG();
		} else {
			int err;
			/* printk(KERN_INFO "C PUD: %lx  (%lx)\n", */
			/*        addr, */
			/*        pud_val(*pud)); */
			err = kmview_patching_pmd_range(pud, addr, opcode, next);
			if (err)
				return err;
		}
	} while (pud++, addr = next, addr < end);

	return 0;
}

static void *kmview_patching(struct kmview *kmview, unsigned long addr, const void *opcode,
				      unsigned long end)
{
    pud_t *pud;
	pud = kmview->pud + pud_index(addr);
	kmview_patching_pud_range(pud, addr, opcode, end);
}





/*
 * Replace instructions with better alternatives for this CPU type. This runs
 * before SMP is initialized to avoid SMP problems with self modifying code.
 * This implies that asymmetric systems where APs have less capabilities than
 * the boot processor are not handled. Tough. Make sure you disable such
 * features by hand.
 *
 * Marked "noinline" to cause control flow change and thus insn cache
 * to refetch changed I$ lines.
 */
void  noinline kmview_apply_alternatives(struct kmview *kmview)
{
	struct kmview_alt_instr *a;
	u8 *instr, *replacement;
	u8 insn_buff[255];
	struct kmview_alt_instr *start = __kmview_alt_instructions;
	struct kmview_alt_instr *end = __kmview_alt_instructions_end;
	DPRINTK("alt table %px, -> %px", start, end);
	/*
	 * The scan order should be from start to end. A later scanned
	 * alternative code can overwrite previously scanned alternative code.
	 * Some kernel functions (e.g. memcpy, memset, etc) use this order to
	 * patch code.
	 *
	 * So be careful if you want to change the scan order to any other
	 * order.
	 */
	for (a = start; a < end; a++) {
		int insn_buff_sz = 0;
		/* Mask away "NOT" flag bit for feature to test. */
		u16 feature = a->cpuid & ~ALTINSTR_FLAG_INV;

		instr = (u8 *)&a->instr_offset + a->instr_offset;
		replacement = (u8 *)&a->repl_offset + a->repl_offset;
		//BUG_ON(a->instrlen > sizeof(insn_buff));
		//BUG_ON(feature != 1);

		//DPRINTK("feat: %s%d*32+%d, old: (%pS (%px) len: %d), repl: (%px, len: %d)",
		//	(a->cpuid & ALTINSTR_FLAG_INV) ? "!" : "",
		//	feature >> 5,
		//	feature & 0x1f,
		//	instr, instr, a->instrlen,
		//	replacement, a->replacementlen);

		//DUMP_BYTES(instr, a->instrlen, "%px:   old_insn: ", instr);
		//DUMP_BYTES(replacement, a->replacementlen, "%px:   rpl_insn: ", replacement);



		memcpy(insn_buff, replacement, a->replacementlen);
		insn_buff_sz = a->replacementlen;

		for (; insn_buff_sz < a->instrlen; insn_buff_sz++)
			insn_buff[insn_buff_sz] = 0x90;


		kmview_patching(kmview, instr, insn_buff, instr + insn_buff_sz);
		//for (; insn_buff_sz < a->instrlen; insn_buff_sz++)
		//	insn_buff[insn_buff_sz] = 0x90;
		//DUMP_BYTES(insn_buff, insn_buff_sz, "%px: final_insn: ", instr);

	}
}
