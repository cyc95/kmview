/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KMVIEW_ALTERNATIVE_H
#define _ASM_X86_KMVIEW_ALTERNATIVE_H

#include <linux/types.h>
#include <linux/stringify.h>
#include <asm/asm.h>

#include <linux/stddef.h>


struct kmview_alt_instr {
	s32 instr_offset;	/* original instruction */
	s32 repl_offset;	/* offset to replacement instruction */
	u16 feature;		/* feature */
	u16  instrlen;		/* length of original instruction */
	u16  replacementlen;	/* length of new instruction */
} __packed;

/*
 * Debug flag that can be tested to see whether alternative
 * instructions were patched in already:
 */

extern void kmview_apply_alternatives(struct alt_instr *start, struct alt_instr *end);



#define b_replacement(num)	"664"#num
#define e_replacement(num)	"665"#num

#define alt_end_marker		"663"
#define alt_slen		"662b-661b"
#define alt_total_slen		alt_end_marker"b-661b"
#define alt_rlen(num)		e_replacement(num)"f-"b_replacement(num)"f"

#define KMVIEW_OLDINSTR(oldinstr, num)						\
	"# ALT: oldnstr\n"						\
	"661:\n\t" oldinstr "\n662:\n"					\
	"# ALT: padding\n"						\
	".skip -(((" alt_rlen(num) ")-(" alt_slen ")) > 0) * "		\
		"((" alt_rlen(num) ")-(" alt_slen ")),0x90\n"		\
	alt_end_marker ":\n"


#define KMVIEW_ALTINSTR_ENTRY(feature, num)					      \
	" .long 661b - .\n"				/* label           */ \
	" .long " b_replacement(num)"f - .\n"		/* new instruction */ \
	" .word " 1 "\n"		/* feature bit     */ \
	" .word " alt_total_slen "\n"			/* source len      */ \
	" .word " alt_rlen(num) "\n"			/* replacement len */

#define KMVIEW_ALTINSTR_REPLACEMENT(newinstr, num)		/* replacement */	\
	"# ALT: replacement " #num "\n"						\
	b_replacement(num)":\n\t" newinstr "\n" e_replacement(num) ":\n"

/* alternative assembly primitive: */
#define KMVIEW_ALTERNATIVE(oldinstr, newinstr, feature)			\
	KMVIEW_OLDINSTR(oldinstr, 1)						\
	".pushsection .kmview_altinstructions,\"a\"\n"				\
	KMVIEW_ALTINSTR_ENTRY(feature, 1)					\
	".popsection\n"							\
	".pushsection .kmview_ altinstr_replacement, \"ax\"\n"			\
	KMVIEW_ALTINSTR_REPLACEMENT(newinstr, 1)				\
	".popsection\n"

#endif /* _ASM_X86_ALTERNATIVE_H */
