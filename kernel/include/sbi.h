#ifndef __SBI_H__
#define __SBI_H__

struct sbiret {
    long error;
    long value;
};

/* SBI BASE extension ID */
#define SBI_EXT_BASE 0x10UL

/* SBI BASE function IDs */
#define SBI_EXT_BASE_GET_SPEC_VERSION   0x0UL
#define SBI_EXT_BASE_GET_IMPL_ID        0x1UL
#define SBI_EXT_BASE_GET_IMPL_VERSION   0x2UL

/* SBI Timer (TIME) extension */
#define SBI_EXT_TIME              0x54494D45UL
#define SBI_EXT_TIME_SET_TIMER    0x0UL

/* SBI v0.1 legacy SET_TIMER (EID = 0x0).  Used as a fallback when the
 * v0.2+ TIME extension is not implemented by the firmware (e.g.
 * SpacemiT K1 OpenSBI returns SBI_ERR_NOT_SUPPORTED for EID 0x54494D45). */
#define SBI_EXT_LEGACY_SET_TIMER  0x0UL

struct sbiret sbi_ecall(unsigned long ext, unsigned long fid,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5);

struct sbiret sbi_get_spec_version(void);
struct sbiret sbi_get_impl_id(void);
struct sbiret sbi_get_impl_version(void);
struct sbiret sbi_set_timer(unsigned long long stime_value);

#endif /* __SBI_H__ */

