/* M6.2a INIT packaging: the 68K INIT code resource (esata_init.flt) + the embedded PowerPC
 * SiI3512 driver PEF (SiI3512SATA.pef, with InstallMe) as a 'PPC ' (128) resource that
 * esata_init.c loads via Get1Resource('PPC ',128) -> GetMemFragment. Both via Rez $$read(). */
#include "Retro68.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
	dontBreakAtEntry, $$read("esata_init.flt");
};

/* The PowerPC SiI3512 driver PEF (exports DoDriverIO + TheDriverDescription + InstallMe),
 * embedded raw so the 68K INIT can GetMemFragment it and call InstallMe. */
data 'PPC ' (128) {
	$$read("SiI3512SATA.pef")
};
