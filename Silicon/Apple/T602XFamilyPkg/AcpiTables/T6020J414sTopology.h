/** @file
  Exact processor topology for the 10-core T6020 J414s.

  This is the single source shared by the MADT and PPTT.  Values are grounded
  in two independent firmware/kernel descriptions:

  * the live J414s Apple Device Tree exposes CPU IDs
    0,1,2,3,4,5,6,8,9,10 and reg values
    0x000-0x003, 0x100-0x102, 0x200-0x202;
  * Asahi Linux t602x-common.dtsi describes P-core MPIDRs with Aff2=1 and
    cluster numbers in Aff1: 0x10100-0x10103 and 0x10200-0x10203.

  Apple ADT's reg property omits the P-core Aff2 bit.  MPIDR_EL1 on the live
  first P-core is 0x10100, so copying ADT reg=0x100 directly into MADT is
  incorrect and makes Windows' GICv3 startup carrier reject CPU4.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/

#ifndef T6020_J414S_TOPOLOGY_H_
#define T6020_J414S_TOPOLOGY_H_

#define T6020_J414S_CPU_COUNT              10
#define T6020_J414S_CLUSTER_COUNT           3
#define T6020_J414S_E_CORE_COUNT            4
#define T6020_J414S_P0_CORE_COUNT           3
#define T6020_J414S_P1_CORE_COUNT           3

#define T6020_J414S_E_UID_BASE              0
#define T6020_J414S_P0_UID_BASE             4
#define T6020_J414S_P1_UID_BASE             7

#define T6020_J414S_E_L2_SIZE       0x00400000
#define T6020_J414S_P0_L2_SIZE      0x01000000
#define T6020_J414S_P1_L2_SIZE      0x01000000

#define T6020_J414S_E_EFFICIENCY_CLASS       0

/*
 * Experimental containment for Windows 26200's first heterogeneous-core
 * bring-up.  Keep the shipping ACPI description unchanged unless the build
 * explicitly opts in: setting the build macro to 1 preserves all ten real
 * MPIDRs and the complete PPTT hierarchy, but reports one homogeneous MADT
 * Processor Power Efficiency Class.  This isolates the scheduler's
 * heterogeneous-class transition from processor-start and topology effects.
 */
#ifndef NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY
#define NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY 0
#endif

#if NTASI_T6020_J414S_HOMOGENEOUS_EFFICIENCY
#define T6020_J414S_P_EFFICIENCY_CLASS       T6020_J414S_E_EFFICIENCY_CLASS
#else
#define T6020_J414S_P_EFFICIENCY_CLASS       1
#endif

/* Linux t6020.dtsi / t602x-common.dtsi native-AIC resources. */
#define T6020_AIC2_CORE_BASE         0x000000028E100000ULL
#define T6020_AIC2_CORE_SIZE         0x000000000000C000ULL
#define T6020_AIC2_EVENT_BASE        0x000000028E10C000ULL
#define T6020_AIC2_EVENT_SIZE        0x0000000000001000ULL
#define T6020_AIC2_IRQ_COUNT         1961

/* Temporary Windows GICv3 startup carrier; not physical Apple MMIO. */
#define T6020_GICD_CARRIER_BASE      0x0000005000000000ULL
#define T6020_GICR_CARRIER_BASE      0x0000005100000000ULL
#define T6020_GICR_FRAME_SIZE        0x00020000U

/* Architected ARM timer PPIs used by Linux and the Windows startup carrier. */
#define T6020_GTDT_SECURE_EL1_GSIV       29
#define T6020_GTDT_NONSECURE_EL1_GSIV    30
#define T6020_GTDT_VIRTUAL_EL1_GSIV      27
#define T6020_GTDT_NONSECURE_EL2_GSIV    26

/*
 * X(LogicalUid, AdtCpuId, Aff2, Aff1, Aff0, EfficiencyClass)
 *
 * ACPI UIDs remain dense for Windows.  AdtCpuId preserves the sparse physical
 * IDs so validation can catch the absent cpu7/cpu11 slots without leaking that
 * sparseness into the OS-visible logical numbering.
 */
#define T6020_J414S_CPU_LIST(X)                                                \
  X (0,  0, 0, 0, 0, T6020_J414S_E_EFFICIENCY_CLASS)                         \
  X (1,  1, 0, 0, 1, T6020_J414S_E_EFFICIENCY_CLASS)                         \
  X (2,  2, 0, 0, 2, T6020_J414S_E_EFFICIENCY_CLASS)                         \
  X (3,  3, 0, 0, 3, T6020_J414S_E_EFFICIENCY_CLASS)                         \
  X (4,  4, 1, 1, 0, T6020_J414S_P_EFFICIENCY_CLASS)                         \
  X (5,  5, 1, 1, 1, T6020_J414S_P_EFFICIENCY_CLASS)                         \
  X (6,  6, 1, 1, 2, T6020_J414S_P_EFFICIENCY_CLASS)                         \
  X (7,  8, 1, 2, 0, T6020_J414S_P_EFFICIENCY_CLASS)                         \
  X (8,  9, 1, 2, 1, T6020_J414S_P_EFFICIENCY_CLASS)                         \
  X (9, 10, 1, 2, 2, T6020_J414S_P_EFFICIENCY_CLASS)

#endif /* T6020_J414S_TOPOLOGY_H_ */
