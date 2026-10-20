// SPDX-License-Identifier: BSD-2-Clause-Patent
// GCC registers used by UFS. SM8550 (sheng) transcribes gcc-sm8550.c; uke is
// SM7675 ("cliffs") and places the UFS clock block at 0x177000 instead of
// 0x77000, with the UFS memory PHY at 0x19e000 instead of 0x9e000. No write is
// performed by NativeUfs yet.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the live uke DT (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

// Window bases inside GCC. SM8550 used 0x77000 / 0x9e000.
#define GCC_UFS_BLOCK_BASE            0x177000
#define GCC_UFS_MEM_PHY_BLOCK_BASE    0x19e000

#define GCC_UFS_PHY_GDSCR             0x177004
#define GCC_UFS_MEM_PHY_GDSCR         0x19e000

#define GCC_UFS_PHY_AXI_ENABLE        0x177018
#define GCC_UFS_PHY_AHB_ENABLE        0x177024
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x177028
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x17702c
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x177068
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x177074
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x1770b0
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x1770cc
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x1770e4, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x1770e4

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8550.c, so CLK_OFF=1 on
// an idle branch is normal (hardware 2026-10-17 read ahb=80000003).
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif
