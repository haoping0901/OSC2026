#include "sbi.h"

struct sbiret sbi_ecall(unsigned long ext, unsigned long fid,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5)
{
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = ext;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    struct sbiret ret = {
        .error = (long)a0,
        .value = (long)a1,
    };
    return ret;
}

struct sbiret sbi_get_spec_version(void)
{
    return sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_SPEC_VERSION,
                     0, 0, 0, 0, 0, 0);
}

struct sbiret sbi_get_impl_id(void)
{
    return sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMPL_ID,
                     0, 0, 0, 0, 0, 0);
}

struct sbiret sbi_get_impl_version(void)
{
    return sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMPL_VERSION,
                     0, 0, 0, 0, 0, 0);
}

/** ----------------------------------------------------------------------
 * @brief sbi_set_timer() – Schedule the next S-mode timer interrupt.
 *
 * Attempts the SBI v0.2+ TIME extension (EID 0x54494D45) first.  When
 * the firmware does not implement it – SpacemiT K1's OpenSBI, for
 * example, returns SBI_ERR_NOT_SUPPORTED – the call is retried using
 * the SBI v0.1 legacy SET_TIMER ecall (EID 0x0), which every OpenSBI
 * build is expected to keep around for backward compatibility.
 * @param stime_value Absolute mtime value at which the next S-timer
 *                    interrupt should fire.
 * @return sbiret of whichever call actually took effect.  On success
 *         .error == 0.  When both paths fail, the legacy call's
 *         sbiret is propagated so the caller can inspect .error.
 * -------------------------------------------------------------------- */
struct sbiret sbi_set_timer(unsigned long long stime_value)
{
	struct sbiret r = sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER,
	                            stime_value, 0, 0, 0, 0, 0);
	if (r.error == 0)
		return r;

	/* Fallback: SBI v0.1 legacy set_timer.  FID is ignored; arg0
	 * carries the full 64-bit mtime value on RV64. */
	return sbi_ecall(SBI_EXT_LEGACY_SET_TIMER, 0,
	                 stime_value, 0, 0, 0, 0, 0);
}

