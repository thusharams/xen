/******************************************************************************
 * xc_cpuid_x86.c 
 *
 * Compute cpuid of a domain.
 *
 * Copyright (c) 2008, Citrix Systems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdbool.h>
#include "xc_private.h"
#include <xen/hvm/params.h>

enum {
#define XEN_CPUFEATURE(name, value) X86_FEATURE_##name = value,
#include <xen/arch-x86/cpufeatureset.h>
};
#include "_xc_cpuid_autogen.h"

#define bitmaskof(idx)      (1u << ((idx) & 31))
#define clear_bit(idx, dst) ((dst) &= ~bitmaskof(idx))
#define set_bit(idx, dst)   ((dst) |=  bitmaskof(idx))

#define DEF_MAX_BASE 0x0000000du
#define DEF_MAX_INTELEXT  0x80000008u
#define DEF_MAX_AMDEXT    0x8000001cu

int xc_get_cpu_levelling_caps(xc_interface *xch, uint32_t *caps)
{
    DECLARE_SYSCTL;
    int ret;

    sysctl.cmd = XEN_SYSCTL_get_cpu_levelling_caps;
    ret = do_sysctl(xch, &sysctl);

    if ( !ret )
        *caps = sysctl.u.cpu_levelling_caps.caps;

    return ret;
}

int xc_get_cpu_featureset(xc_interface *xch, uint32_t index,
                          uint32_t *nr_features, uint32_t *featureset)
{
    DECLARE_SYSCTL;
    DECLARE_HYPERCALL_BOUNCE(featureset,
                             *nr_features * sizeof(*featureset),
                             XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int ret;

    if ( xc_hypercall_bounce_pre(xch, featureset) )
        return -1;

    sysctl.cmd = XEN_SYSCTL_get_cpu_featureset;
    sysctl.u.cpu_featureset.index = index;
    sysctl.u.cpu_featureset.nr_features = *nr_features;
    set_xen_guest_handle(sysctl.u.cpu_featureset.features, featureset);

    ret = do_sysctl(xch, &sysctl);

    xc_hypercall_bounce_post(xch, featureset);

    if ( !ret )
        *nr_features = sysctl.u.cpu_featureset.nr_features;

    return ret;
}

uint32_t xc_get_cpu_featureset_size(void)
{
    return FEATURESET_NR_ENTRIES;
}

const uint32_t *xc_get_static_cpu_featuremask(
    enum xc_static_cpu_featuremask mask)
{
    const static uint32_t known[FEATURESET_NR_ENTRIES] = INIT_KNOWN_FEATURES,
        special[FEATURESET_NR_ENTRIES] = INIT_SPECIAL_FEATURES,
        pv[FEATURESET_NR_ENTRIES] = INIT_PV_FEATURES,
        hvm_shadow[FEATURESET_NR_ENTRIES] = INIT_HVM_SHADOW_FEATURES,
        hvm_hap[FEATURESET_NR_ENTRIES] = INIT_HVM_HAP_FEATURES,
        deep_features[FEATURESET_NR_ENTRIES] = INIT_DEEP_FEATURES;

    XC_BUILD_BUG_ON(ARRAY_SIZE(known) != FEATURESET_NR_ENTRIES);
    XC_BUILD_BUG_ON(ARRAY_SIZE(special) != FEATURESET_NR_ENTRIES);
    XC_BUILD_BUG_ON(ARRAY_SIZE(pv) != FEATURESET_NR_ENTRIES);
    XC_BUILD_BUG_ON(ARRAY_SIZE(hvm_shadow) != FEATURESET_NR_ENTRIES);
    XC_BUILD_BUG_ON(ARRAY_SIZE(hvm_hap) != FEATURESET_NR_ENTRIES);
    XC_BUILD_BUG_ON(ARRAY_SIZE(deep_features) != FEATURESET_NR_ENTRIES);

    switch ( mask )
    {
    case XC_FEATUREMASK_KNOWN:
        return known;

    case XC_FEATUREMASK_SPECIAL:
        return special;

    case XC_FEATUREMASK_PV:
        return pv;

    case XC_FEATUREMASK_HVM_SHADOW:
        return hvm_shadow;

    case XC_FEATUREMASK_HVM_HAP:
        return hvm_hap;

    case XC_FEATUREMASK_DEEP_FEATURES:
        return deep_features;

    default:
        return NULL;
    }
}

const uint32_t *xc_get_feature_deep_deps(uint32_t feature)
{
    static const struct {
        uint32_t feature;
        uint32_t fs[FEATURESET_NR_ENTRIES];
    } deep_deps[] = INIT_DEEP_DEPS;

    unsigned int start = 0, end = ARRAY_SIZE(deep_deps);

    XC_BUILD_BUG_ON(ARRAY_SIZE(deep_deps) != NR_DEEP_DEPS);

    /* deep_deps[] is sorted.  Perform a binary search. */
    while ( start < end )
    {
        unsigned int mid = start + ((end - start) / 2);

        if ( deep_deps[mid].feature > feature )
            end = mid;
        else if ( deep_deps[mid].feature < feature )
            start = mid + 1;
        else
            return deep_deps[mid].fs;
    }

    return NULL;
}

struct cpuid_domain_info
{
    enum
    {
        VENDOR_UNKNOWN,
        VENDOR_INTEL,
        VENDOR_AMD,
    } vendor;

    bool hvm;
    bool pvh;
    uint64_t xfeature_mask;

    uint32_t *featureset;
    unsigned int nr_features;

    /* PV-only information. */
    bool pv64;

    /* HVM-only information. */
    bool pae;
    bool nestedhvm;
};

static void cpuid(const unsigned int *input, unsigned int *regs)
{
    unsigned int count = (input[1] == XEN_CPUID_INPUT_UNUSED) ? 0 : input[1];
#ifdef __i386__
    /* Use the stack to avoid reg constraint failures with some gcc flags */
    asm (
        "push %%ebx; push %%edx\n\t"
        "cpuid\n\t"
        "mov %%ebx,4(%4)\n\t"
        "mov %%edx,12(%4)\n\t"
        "pop %%edx; pop %%ebx\n\t"
        : "=a" (regs[0]), "=c" (regs[2])
        : "0" (input[0]), "1" (count), "S" (regs)
        : "memory" );
#else
    asm (
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "0" (input[0]), "2" (count) );
#endif
}

static int get_cpuid_domain_info(xc_interface *xch, domid_t domid,
                                 struct cpuid_domain_info *info,
                                 uint32_t *featureset,
                                 unsigned int nr_features)
{
    struct xen_domctl domctl = {};
    xc_dominfo_t di;
    unsigned int in[2] = { 0, ~0U }, regs[4];
    unsigned int i, host_nr_features = xc_get_cpu_featureset_size();
    int rc;

    cpuid(in, regs);
    if ( regs[1] == 0x756e6547U &&      /* "GenuineIntel" */
         regs[2] == 0x6c65746eU &&
         regs[3] == 0x49656e69U )
        info->vendor = VENDOR_INTEL;
    else if ( regs[1] == 0x68747541U && /* "AuthenticAMD" */
              regs[2] == 0x444d4163U &&
              regs[3] == 0x69746e65U )
        info->vendor = VENDOR_AMD;
    else
        info->vendor = VENDOR_UNKNOWN;

    if ( xc_domain_getinfo(xch, domid, 1, &di) != 1 ||
         di.domid != domid )
        return -ESRCH;

    info->hvm = di.hvm;
    info->pvh = di.pvh;

    info->featureset = calloc(host_nr_features, sizeof(*info->featureset));
    if ( !info->featureset )
        return -ENOMEM;

    info->nr_features = host_nr_features;

    if ( featureset )
    {
        memcpy(info->featureset, featureset,
               min(host_nr_features, nr_features) * sizeof(*info->featureset));

        /* Check for truncated set bits. */
        for ( i = nr_features; i < host_nr_features; ++i )
            if ( featureset[i] != 0 )
                return -EOPNOTSUPP;
    }

    /* Get xstate information. */
    domctl.cmd = XEN_DOMCTL_getvcpuextstate;
    domctl.domain = domid;
    rc = do_domctl(xch, &domctl);
    if ( rc )
        return rc;

    info->xfeature_mask = domctl.u.vcpuextstate.xfeature_mask;

    if ( di.hvm )
    {
        uint64_t val;

        rc = xc_hvm_param_get(xch, domid, HVM_PARAM_PAE_ENABLED, &val);
        if ( rc )
            return rc;

        info->pae = !!val;

        rc = xc_hvm_param_get(xch, domid, HVM_PARAM_NESTEDHVM, &val);
        if ( rc )
            return rc;

        info->nestedhvm = !!val;

        if ( !featureset )
        {
            rc = xc_get_cpu_featureset(xch, XEN_SYSCTL_cpu_featureset_hvm,
                                       &host_nr_features, info->featureset);
            if ( rc )
                return rc;
        }
    }
    else
    {
        unsigned int width;

        rc = xc_domain_get_guest_width(xch, domid, &width);
        if ( rc )
            return rc;

        info->pv64 = (width == 8);

        if ( !featureset )
        {
            rc = xc_get_cpu_featureset(xch, XEN_SYSCTL_cpu_featureset_pv,
                                       &host_nr_features, info->featureset);
            if ( rc )
                return rc;
        }
    }

    return 0;
}

static void free_cpuid_domain_info(struct cpuid_domain_info *info)
{
    free(info->featureset);
}

static void amd_xc_cpuid_policy(xc_interface *xch,
                                const struct cpuid_domain_info *info,
                                const unsigned int *input, unsigned int *regs)
{
    switch ( input[0] )
    {
    case 0x00000002:
    case 0x00000004:
        regs[0] = regs[1] = regs[2] = 0;
        break;

    case 0x80000000:
        if ( regs[0] > DEF_MAX_AMDEXT )
            regs[0] = DEF_MAX_AMDEXT;
        break;

    case 0x80000001: {
        if ( !info->pae )
            clear_bit(X86_FEATURE_PAE, regs[3]);

        /* Filter all other features according to a whitelist. */
        regs[2] &= (bitmaskof(X86_FEATURE_LAHF_LM) |
                    bitmaskof(X86_FEATURE_CMP_LEGACY) |
                    (info->nestedhvm ? bitmaskof(X86_FEATURE_SVM) : 0) |
                    bitmaskof(X86_FEATURE_CR8_LEGACY) |
                    bitmaskof(X86_FEATURE_ABM) |
                    bitmaskof(X86_FEATURE_SSE4A) |
                    bitmaskof(X86_FEATURE_MISALIGNSSE) |
                    bitmaskof(X86_FEATURE_3DNOWPREFETCH) |
                    bitmaskof(X86_FEATURE_OSVW) |
                    bitmaskof(X86_FEATURE_XOP) |
                    bitmaskof(X86_FEATURE_LWP) |
                    bitmaskof(X86_FEATURE_FMA4) |
                    bitmaskof(X86_FEATURE_TBM) |
                    bitmaskof(X86_FEATURE_DBEXT));
        regs[3] &= (0x0183f3ff | /* features shared with 0x00000001:EDX */
                    bitmaskof(X86_FEATURE_NX) |
                    bitmaskof(X86_FEATURE_LM) |
                    bitmaskof(X86_FEATURE_PAGE1GB) |
                    bitmaskof(X86_FEATURE_SYSCALL) |
                    bitmaskof(X86_FEATURE_MMXEXT) |
                    bitmaskof(X86_FEATURE_FFXSR) |
                    bitmaskof(X86_FEATURE_3DNOW) |
                    bitmaskof(X86_FEATURE_3DNOWEXT));
        break;
    }

    case 0x80000008:
        /*
         * ECX[15:12] is ApicIdCoreSize: ECX[7:0] is NumberOfCores (minus one).
         * Update to reflect vLAPIC_ID = vCPU_ID * 2.
         */
        regs[2] = ((regs[2] & 0xf000u) + 1) | ((regs[2] & 0xffu) << 1) | 1u;
        break;

    case 0x8000000a: {
        if ( !info->nestedhvm )
        {
            regs[0] = regs[1] = regs[2] = regs[3] = 0;
            break;
        }

#define SVM_FEATURE_NPT            0x00000001 /* Nested page table support */
#define SVM_FEATURE_LBRV           0x00000002 /* LBR virtualization support */
#define SVM_FEATURE_SVML           0x00000004 /* SVM locking MSR support */
#define SVM_FEATURE_NRIPS          0x00000008 /* Next RIP save on VMEXIT */
#define SVM_FEATURE_TSCRATEMSR     0x00000010 /* TSC ratio MSR support */
#define SVM_FEATURE_VMCBCLEAN      0x00000020 /* VMCB clean bits support */
#define SVM_FEATURE_FLUSHBYASID    0x00000040 /* TLB flush by ASID support */
#define SVM_FEATURE_DECODEASSISTS  0x00000080 /* Decode assists support */
#define SVM_FEATURE_PAUSEFILTER    0x00000400 /* Pause intercept filter */

        /* Pass 1: Only passthrough SVM features which are
         * available in hw and which are implemented
         */
        regs[3] &= (SVM_FEATURE_NPT | SVM_FEATURE_LBRV | \
            SVM_FEATURE_NRIPS | SVM_FEATURE_PAUSEFILTER | \
            SVM_FEATURE_DECODEASSISTS);

        /* Pass 2: Always enable SVM features which are emulated */
        regs[3] |= SVM_FEATURE_VMCBCLEAN | SVM_FEATURE_TSCRATEMSR;
        break;
    }

    }
}

static void intel_xc_cpuid_policy(xc_interface *xch,
                                  const struct cpuid_domain_info *info,
                                  const unsigned int *input, unsigned int *regs)
{
    switch ( input[0] )
    {
    case 0x00000001:
        /* ECX[5] is availability of VMX */
        if ( info->nestedhvm )
            set_bit(X86_FEATURE_VMX, regs[2]);
        break;

    case 0x00000004:
        /*
         * EAX[31:26] is Maximum Cores Per Package (minus one).
         * Update to reflect vLAPIC_ID = vCPU_ID * 2.
         */
        regs[0] = (((regs[0] & 0x7c000000u) << 1) | 0x04000000u |
                   (regs[0] & 0x3ffu));
        regs[3] &= 0x3ffu;
        break;

    case 0x80000000:
        if ( regs[0] > DEF_MAX_INTELEXT )
            regs[0] = DEF_MAX_INTELEXT;
        break;

    case 0x80000001: {
        /* Only a few features are advertised in Intel's 0x80000001. */
        regs[2] &= (bitmaskof(X86_FEATURE_LAHF_LM) |
                    bitmaskof(X86_FEATURE_3DNOWPREFETCH) |
                    bitmaskof(X86_FEATURE_ABM));
        regs[3] &= (bitmaskof(X86_FEATURE_NX) |
                    bitmaskof(X86_FEATURE_LM) |
                    bitmaskof(X86_FEATURE_PAGE1GB) |
                    bitmaskof(X86_FEATURE_SYSCALL) |
                    bitmaskof(X86_FEATURE_RDTSCP));
        break;
    }

    case 0x80000005:
        regs[0] = regs[1] = regs[2] = 0;
        break;

    case 0x80000008:
        /* Mask AMD Number of Cores information. */
        regs[2] = 0;
        break;
    }
}

#define XSAVEOPT        (1 << 0)
#define XSAVEC          (1 << 1)
#define XGETBV1         (1 << 2)
#define XSAVES          (1 << 3)
/* Configure extended state enumeration leaves (0x0000000D for xsave) */
static void xc_cpuid_config_xsave(xc_interface *xch,
                                  const struct cpuid_domain_info *info,
                                  const unsigned int *input, unsigned int *regs)
{
    if ( info->xfeature_mask == 0 )
    {
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
        return;
    }

    switch ( input[1] )
    {
    case 0: 
        /* EAX: low 32bits of xfeature_enabled_mask */
        regs[0] = info->xfeature_mask & 0xFFFFFFFF;
        /* EDX: high 32bits of xfeature_enabled_mask */
        regs[3] = (info->xfeature_mask >> 32) & 0xFFFFFFFF;
        /* ECX: max size required by all HW features */
        {
            unsigned int _input[2] = {0xd, 0x0}, _regs[4];
            regs[2] = 0;
            for ( _input[1] = 2; _input[1] < 64; _input[1]++ )
            {
                cpuid(_input, _regs);
                if ( (_regs[0] + _regs[1]) > regs[2] )
                    regs[2] = _regs[0] + _regs[1];
            }
        }
        /* EBX: max size required by enabled features. 
         * This register contains a dynamic value, which varies when a guest 
         * enables or disables XSTATE features (via xsetbv). The default size 
         * after reset is 576. */ 
        regs[1] = 512 + 64; /* FP/SSE + XSAVE.HEADER */
        break;
    case 1: /* leaf 1 */
        regs[0] &= (XSAVEOPT | XSAVEC | XGETBV1 | XSAVES);
        if ( !info->hvm )
            regs[0] &= ~XSAVES;
        regs[2] &= info->xfeature_mask;
        regs[3] = 0;
        break;
    case 2 ... 63: /* sub-leaves */
        if ( !(info->xfeature_mask & (1ULL << input[1])) )
        {
            regs[0] = regs[1] = regs[2] = regs[3] = 0;
            break;
        }
        /* Don't touch EAX, EBX. Also cleanup ECX and EDX */
        regs[2] = regs[3] = 0;
        break;
    }
}

static void xc_cpuid_hvm_policy(xc_interface *xch,
                                const struct cpuid_domain_info *info,
                                const unsigned int *input, unsigned int *regs)
{
    switch ( input[0] )
    {
    case 0x00000000:
        if ( regs[0] > DEF_MAX_BASE )
            regs[0] = DEF_MAX_BASE;
        break;

    case 0x00000001:
        /*
         * EBX[23:16] is Maximum Logical Processors Per Package.
         * Update to reflect vLAPIC_ID = vCPU_ID * 2.
         */
        regs[1] = (regs[1] & 0x0000ffffu) | ((regs[1] & 0x007f0000u) << 1);

        regs[2] &= (bitmaskof(X86_FEATURE_SSE3) |
                    bitmaskof(X86_FEATURE_PCLMULQDQ) |
                    bitmaskof(X86_FEATURE_SSSE3) |
                    bitmaskof(X86_FEATURE_FMA) |
                    bitmaskof(X86_FEATURE_CX16) |
                    bitmaskof(X86_FEATURE_PCID) |
                    bitmaskof(X86_FEATURE_SSE4_1) |
                    bitmaskof(X86_FEATURE_SSE4_2) |
                    bitmaskof(X86_FEATURE_MOVBE)  |
                    bitmaskof(X86_FEATURE_POPCNT) |
                    bitmaskof(X86_FEATURE_AESNI) |
                    bitmaskof(X86_FEATURE_F16C) |
                    bitmaskof(X86_FEATURE_RDRAND) |
                    ((info->xfeature_mask != 0) ?
                     (bitmaskof(X86_FEATURE_AVX) |
                      bitmaskof(X86_FEATURE_XSAVE)) : 0));

        regs[2] |= (bitmaskof(X86_FEATURE_HYPERVISOR) |
                    bitmaskof(X86_FEATURE_TSC_DEADLINE) |
                    bitmaskof(X86_FEATURE_X2APIC));

        regs[3] &= (bitmaskof(X86_FEATURE_FPU) |
                    bitmaskof(X86_FEATURE_VME) |
                    bitmaskof(X86_FEATURE_DE) |
                    bitmaskof(X86_FEATURE_PSE) |
                    bitmaskof(X86_FEATURE_TSC) |
                    bitmaskof(X86_FEATURE_MSR) |
                    bitmaskof(X86_FEATURE_PAE) |
                    bitmaskof(X86_FEATURE_MCE) |
                    bitmaskof(X86_FEATURE_CX8) |
                    bitmaskof(X86_FEATURE_APIC) |
                    bitmaskof(X86_FEATURE_SEP) |
                    bitmaskof(X86_FEATURE_MTRR) |
                    bitmaskof(X86_FEATURE_PGE) |
                    bitmaskof(X86_FEATURE_MCA) |
                    bitmaskof(X86_FEATURE_CMOV) |
                    bitmaskof(X86_FEATURE_PAT) |
                    bitmaskof(X86_FEATURE_CLFLUSH) |
                    bitmaskof(X86_FEATURE_PSE36) |
                    bitmaskof(X86_FEATURE_MMX) |
                    bitmaskof(X86_FEATURE_FXSR) |
                    bitmaskof(X86_FEATURE_SSE) |
                    bitmaskof(X86_FEATURE_SSE2) |
                    bitmaskof(X86_FEATURE_HTT));
            
        /* We always support MTRR MSRs. */
        regs[3] |= bitmaskof(X86_FEATURE_MTRR);

        if ( !info->pae )
        {
            clear_bit(X86_FEATURE_PAE, regs[3]);
            clear_bit(X86_FEATURE_PSE36, regs[3]);
        }
        break;

    case 0x00000007: /* Intel-defined CPU features */
        if ( input[1] == 0 ) {
            regs[1] &= (bitmaskof(X86_FEATURE_TSC_ADJUST) |
                        bitmaskof(X86_FEATURE_BMI1) |
                        bitmaskof(X86_FEATURE_HLE)  |
                        bitmaskof(X86_FEATURE_AVX2) |
                        bitmaskof(X86_FEATURE_SMEP) |
                        bitmaskof(X86_FEATURE_BMI2) |
                        bitmaskof(X86_FEATURE_ERMS) |
                        bitmaskof(X86_FEATURE_INVPCID) |
                        bitmaskof(X86_FEATURE_RTM)  |
                        ((info->xfeature_mask != 0) ?
                        bitmaskof(X86_FEATURE_MPX) : 0)  |
                        bitmaskof(X86_FEATURE_RDSEED)  |
                        bitmaskof(X86_FEATURE_ADX)  |
                        bitmaskof(X86_FEATURE_SMAP) |
                        bitmaskof(X86_FEATURE_FSGSBASE) |
                        bitmaskof(X86_FEATURE_PCOMMIT) |
                        bitmaskof(X86_FEATURE_CLWB) |
                        bitmaskof(X86_FEATURE_CLFLUSHOPT));
            regs[2] &= bitmaskof(X86_FEATURE_PKU);
        } else
            regs[1] = regs[2] = 0;

        regs[0] = regs[3] = 0;
        break;

    case 0x0000000d:
        xc_cpuid_config_xsave(xch, info, input, regs);
        break;

    case 0x80000000:
        /* Passthrough to cpu vendor specific functions */
        break;

    case 0x80000001:
        if ( !info->pae )
        {
            clear_bit(X86_FEATURE_LAHF_LM, regs[2]);
            clear_bit(X86_FEATURE_LM, regs[3]);
            clear_bit(X86_FEATURE_NX, regs[3]);
            clear_bit(X86_FEATURE_PSE36, regs[3]);
            clear_bit(X86_FEATURE_PAGE1GB, regs[3]);
        }
        break;

    case 0x80000007:
        /*
         * Keep only TSCInvariant. This may be cleared by the hypervisor
         * depending on guest TSC and migration settings.
         */
        regs[0] = regs[1] = regs[2] = 0;
        regs[3] &= 1u<<8;
        break;

    case 0x80000008:
        regs[0] &= 0x0000ffffu;
        regs[1] = regs[3] = 0;
        break;

    case 0x00000002: /* Intel cache info (dumped by AMD policy) */
    case 0x00000004: /* Intel cache info (dumped by AMD policy) */
    case 0x0000000a: /* Architectural Performance Monitor Features */
    case 0x80000002: /* Processor name string */
    case 0x80000003: /* ... continued         */
    case 0x80000004: /* ... continued         */
    case 0x80000005: /* AMD L1 cache/TLB info (dumped by Intel policy) */
    case 0x80000006: /* AMD L2/3 cache/TLB info ; Intel L2 cache features */
    case 0x8000000a: /* AMD SVM feature bits */
    case 0x8000001c: /* AMD lightweight profiling */
        break;

    default:
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
        break;
    }

    if ( info->vendor == VENDOR_AMD )
        amd_xc_cpuid_policy(xch, info, input, regs);
    else
        intel_xc_cpuid_policy(xch, info, input, regs);
}

static void xc_cpuid_pv_policy(xc_interface *xch,
                               const struct cpuid_domain_info *info,
                               const unsigned int *input, unsigned int *regs)
{
    if ( (input[0] & 0x7fffffff) == 0x00000001 )
    {
        clear_bit(X86_FEATURE_VME, regs[3]);
        if ( !info->pvh )
        {
            clear_bit(X86_FEATURE_PSE, regs[3]);
            clear_bit(X86_FEATURE_PGE, regs[3]);
        }
        clear_bit(X86_FEATURE_MCE, regs[3]);
        clear_bit(X86_FEATURE_MCA, regs[3]);
        clear_bit(X86_FEATURE_MTRR, regs[3]);
        clear_bit(X86_FEATURE_PSE36, regs[3]);
    }

    switch ( input[0] )
    {
    case 0x00000001:
        if ( info->vendor == VENDOR_AMD )
            clear_bit(X86_FEATURE_SEP, regs[3]);
        clear_bit(X86_FEATURE_DS, regs[3]);
        clear_bit(X86_FEATURE_TM1, regs[3]);
        clear_bit(X86_FEATURE_PBE, regs[3]);

        clear_bit(X86_FEATURE_DTES64, regs[2]);
        clear_bit(X86_FEATURE_MONITOR, regs[2]);
        clear_bit(X86_FEATURE_DSCPL, regs[2]);
        clear_bit(X86_FEATURE_VMX, regs[2]);
        clear_bit(X86_FEATURE_SMX, regs[2]);
        clear_bit(X86_FEATURE_EIST, regs[2]);
        clear_bit(X86_FEATURE_TM2, regs[2]);
        if ( !info->pv64 )
            clear_bit(X86_FEATURE_CX16, regs[2]);
        if ( info->xfeature_mask == 0 )
        {
            clear_bit(X86_FEATURE_XSAVE, regs[2]);
            clear_bit(X86_FEATURE_AVX, regs[2]);
        }
        clear_bit(X86_FEATURE_XTPR, regs[2]);
        clear_bit(X86_FEATURE_PDCM, regs[2]);
        clear_bit(X86_FEATURE_PCID, regs[2]);
        clear_bit(X86_FEATURE_DCA, regs[2]);
        set_bit(X86_FEATURE_HYPERVISOR, regs[2]);
        break;

    case 0x00000007:
        if ( input[1] == 0 )
        {
            regs[1] &= (bitmaskof(X86_FEATURE_BMI1) |
                        bitmaskof(X86_FEATURE_HLE)  |
                        bitmaskof(X86_FEATURE_AVX2) |
                        bitmaskof(X86_FEATURE_BMI2) |
                        bitmaskof(X86_FEATURE_ERMS) |
                        bitmaskof(X86_FEATURE_RTM)  |
                        bitmaskof(X86_FEATURE_RDSEED)  |
                        bitmaskof(X86_FEATURE_ADX)  |
                        bitmaskof(X86_FEATURE_FSGSBASE));
            if ( info->xfeature_mask == 0 )
                clear_bit(X86_FEATURE_MPX, regs[1]);
        }
        else
            regs[1] = 0;
        regs[0] = regs[2] = regs[3] = 0;
        break;

    case 0x0000000d:
        xc_cpuid_config_xsave(xch, info, input, regs);
        break;

    case 0x80000001:
        if ( !info->pv64 )
        {
            clear_bit(X86_FEATURE_LM, regs[3]);
            clear_bit(X86_FEATURE_LAHF_LM, regs[2]);
            if ( info->vendor != VENDOR_AMD )
                clear_bit(X86_FEATURE_SYSCALL, regs[3]);
        }
        else
        {
            set_bit(X86_FEATURE_SYSCALL, regs[3]);
        }
        if ( !info->pvh )
            clear_bit(X86_FEATURE_PAGE1GB, regs[3]);
        clear_bit(X86_FEATURE_RDTSCP, regs[3]);

        clear_bit(X86_FEATURE_SVM, regs[2]);
        clear_bit(X86_FEATURE_OSVW, regs[2]);
        clear_bit(X86_FEATURE_IBS, regs[2]);
        clear_bit(X86_FEATURE_SKINIT, regs[2]);
        clear_bit(X86_FEATURE_WDT, regs[2]);
        clear_bit(X86_FEATURE_LWP, regs[2]);
        clear_bit(X86_FEATURE_NODEID_MSR, regs[2]);
        clear_bit(X86_FEATURE_TOPOEXT, regs[2]);
        break;

    case 0x00000005: /* MONITOR/MWAIT */
    case 0x0000000a: /* Architectural Performance Monitor Features */
    case 0x0000000b: /* Extended Topology Enumeration */
    case 0x8000000a: /* SVM revision and features */
    case 0x8000001b: /* Instruction Based Sampling */
    case 0x8000001c: /* Light Weight Profiling */
    case 0x8000001e: /* Extended topology reporting */
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
        break;
    }
}

static int xc_cpuid_policy(xc_interface *xch,
                           const struct cpuid_domain_info *info,
                           const unsigned int *input, unsigned int *regs)
{
    /*
     * For hypervisor leaves (0x4000XXXX) only 0x4000xx00.EAX[7:0] bits (max
     * number of leaves) can be set by user. Hypervisor will enforce this so
     * all other bits are don't-care and we can set them to zero.
     */
    if ( (input[0] & 0xffff0000) == 0x40000000 )
    {
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
        return 0;
    }

    if ( info->hvm )
        xc_cpuid_hvm_policy(xch, info, input, regs);
    else
        xc_cpuid_pv_policy(xch, info, input, regs);

    return 0;
}

static int xc_cpuid_do_domctl(
    xc_interface *xch, domid_t domid,
    const unsigned int *input, const unsigned int *regs)
{
    DECLARE_DOMCTL;

    memset(&domctl, 0, sizeof (domctl));
    domctl.domain = domid;
    domctl.cmd = XEN_DOMCTL_set_cpuid;
    domctl.u.cpuid.input[0] = input[0];
    domctl.u.cpuid.input[1] = input[1];
    domctl.u.cpuid.eax = regs[0];
    domctl.u.cpuid.ebx = regs[1];
    domctl.u.cpuid.ecx = regs[2];
    domctl.u.cpuid.edx = regs[3];

    return do_domctl(xch, &domctl);
}

static char *alloc_str(void)
{
    char *s = malloc(33);
    if ( s == NULL )
        return s;
    memset(s, 0, 33);
    return s;
}

void xc_cpuid_to_str(const unsigned int *regs, char **strs)
{
    int i, j;

    for ( i = 0; i < 4; i++ )
    {
        strs[i] = alloc_str();
        if ( strs[i] == NULL )
            continue;
        for ( j = 0; j < 32; j++ )
            strs[i][j] = !!((regs[i] & (1U << (31 - j)))) ? '1' : '0';
    }
}

int xc_cpuid_apply_policy(xc_interface *xch, domid_t domid,
                          uint32_t *featureset,
                          unsigned int nr_features)
{
    struct cpuid_domain_info info = {};
    unsigned int input[2] = { 0, 0 }, regs[4];
    unsigned int base_max, ext_max;
    int rc;

    rc = get_cpuid_domain_info(xch, domid, &info, featureset, nr_features);
    if ( rc )
        goto out;

    cpuid(input, regs);
    base_max = (regs[0] <= DEF_MAX_BASE) ? regs[0] : DEF_MAX_BASE;
    input[0] = 0x80000000;
    cpuid(input, regs);

    if ( info.vendor == VENDOR_AMD )
        ext_max = (regs[0] <= DEF_MAX_AMDEXT) ? regs[0] : DEF_MAX_AMDEXT;
    else
        ext_max = (regs[0] <= DEF_MAX_INTELEXT) ? regs[0] : DEF_MAX_INTELEXT;

    input[0] = 0;
    input[1] = XEN_CPUID_INPUT_UNUSED;
    for ( ; ; )
    {
        cpuid(input, regs);
        xc_cpuid_policy(xch, &info, input, regs);

        if ( regs[0] || regs[1] || regs[2] || regs[3] )
        {
            rc = xc_cpuid_do_domctl(xch, domid, input, regs);
            if ( rc )
                goto out;
        }

        /* Intel cache descriptor leaves. */
        if ( input[0] == 4 )
        {
            input[1]++;
            /* More to do? Then loop keeping %%eax==0x00000004. */
            if ( (regs[0] & 0x1f) != 0 )
                continue;
        }

        /* XSAVE information, subleaves 0-63. */
        if ( (input[0] == 0xd) && (input[1]++ < 63) )
            continue;

        input[0]++;
        if ( !(input[0] & 0x80000000u) && (input[0] > base_max ) )
            input[0] = 0x80000000u;

        input[1] = XEN_CPUID_INPUT_UNUSED;
        if ( (input[0] == 4) || (input[0] == 7) || (input[0] == 0xd) )
            input[1] = 0;

        if ( (input[0] & 0x80000000u) && (input[0] > ext_max) )
            break;
    }

 out:
    free_cpuid_domain_info(&info);
    return rc;
}

/*
 * Check whether a VM is allowed to launch on this host's processor type.
 *
 * @config format is similar to that of xc_cpuid_set():
 *  '1' -> the bit must be set to 1
 *  '0' -> must be 0
 *  'x' -> we don't care
 *  's' -> (same) must be the same
 */
int xc_cpuid_check(
    xc_interface *xch, const unsigned int *input,
    const char **config,
    char **config_transformed)
{
    int i, j, rc;
    unsigned int regs[4];

    memset(config_transformed, 0, 4 * sizeof(*config_transformed));

    cpuid(input, regs);

    for ( i = 0; i < 4; i++ )
    {
        if ( config[i] == NULL )
            continue;
        config_transformed[i] = alloc_str();
        if ( config_transformed[i] == NULL )
        {
            rc = -ENOMEM;
            goto fail_rc;
        }
        for ( j = 0; j < 32; j++ )
        {
            unsigned char val = !!((regs[i] & (1U << (31 - j))));
            if ( !strchr("10xs", config[i][j]) ||
                 ((config[i][j] == '1') && !val) ||
                 ((config[i][j] == '0') && val) )
                goto fail;
            config_transformed[i][j] = config[i][j];
            if ( config[i][j] == 's' )
                config_transformed[i][j] = '0' + val;
        }
    }

    return 0;

 fail:
    rc = -EPERM;
 fail_rc:
    for ( i = 0; i < 4; i++ )
    {
        free(config_transformed[i]);
        config_transformed[i] = NULL;
    }
    return rc;
}

/*
 * Configure a single input with the informatiom from config.
 *
 * Config is an array of strings:
 *   config[0] = eax
 *   config[1] = ebx
 *   config[2] = ecx
 *   config[3] = edx
 *
 * The format of the string is the following:
 *   '1' -> force to 1
 *   '0' -> force to 0
 *   'x' -> we don't care (use default)
 *   'k' -> pass through host value
 *   's' -> pass through the first time and then keep the same value
 *          across save/restore and migration.
 * 
 * For 's' and 'x' the configuration is overwritten with the value applied.
 */
int xc_cpuid_set(
    xc_interface *xch, domid_t domid, const unsigned int *input,
    const char **config, char **config_transformed)
{
    int rc;
    unsigned int i, j, regs[4], polregs[4];
    struct cpuid_domain_info info = {};

    memset(config_transformed, 0, 4 * sizeof(*config_transformed));

    rc = get_cpuid_domain_info(xch, domid, &info, NULL, 0);
    if ( rc )
        goto out;

    cpuid(input, regs);

    memcpy(polregs, regs, sizeof(regs));
    xc_cpuid_policy(xch, &info, input, polregs);

    for ( i = 0; i < 4; i++ )
    {
        if ( config[i] == NULL )
        {
            regs[i] = polregs[i];
            continue;
        }
        
        config_transformed[i] = alloc_str();
        if ( config_transformed[i] == NULL )
        {
            rc = -ENOMEM;
            goto fail;
        }

        for ( j = 0; j < 32; j++ )
        {
            unsigned char val = !!((regs[i] & (1U << (31 - j))));
            unsigned char polval = !!((polregs[i] & (1U << (31 - j))));

            rc = -EINVAL;
            if ( !strchr("10xks", config[i][j]) )
                goto fail;

            if ( config[i][j] == '1' )
                val = 1;
            else if ( config[i][j] == '0' )
                val = 0;
            else if ( config[i][j] == 'x' )
                val = polval;

            if ( val )
                set_bit(31 - j, regs[i]);
            else
                clear_bit(31 - j, regs[i]);

            config_transformed[i][j] = config[i][j];
            if ( config[i][j] == 's' )
                config_transformed[i][j] = '0' + val;
        }
    }

    rc = xc_cpuid_do_domctl(xch, domid, input, regs);
    if ( rc == 0 )
        goto out;

 fail:
    for ( i = 0; i < 4; i++ )
    {
        free(config_transformed[i]);
        config_transformed[i] = NULL;
    }

 out:
    free_cpuid_domain_info(&info);
    return rc;
}
