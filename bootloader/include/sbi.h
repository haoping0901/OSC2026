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

struct sbiret sbi_ecall(unsigned long ext, unsigned long fid,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5);

struct sbiret sbi_get_spec_version(void);
struct sbiret sbi_get_impl_id(void);
struct sbiret sbi_get_impl_version(void);

#endif /* __SBI_H__ */

