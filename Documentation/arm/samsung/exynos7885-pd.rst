Exynos7885/7904 DISPAUD bring-up
==============================

Scope and handover
------------------

EXYNOS7885_PM_DOMAINS registers only the DISPAUD genpd provider. The provider
reads PMU 0x11c80000 + 0x4024, mask 0xf, before pm_genpd_init(): 0xf means ON,
zero means OFF, and a partial value fails probe. Registration does not write
power registers, read the live display's save list, or issue an SMC. With no
attached consumers, power_off returns -EBUSY, so late unused-domain cleanup
cannot turn off the bootloader splash. Once consumers attach, normal genpd
runtime-PM accounting controls transitions. There is no unconditional boot
power-off or always-on flag that would prevent a real consumer from suspending.

The board enables the provider, but currently has no DECON, DSIM, DPP or ABOX
consumer nodes. The firmware framebuffer, UART and eMMC are intentionally not
attached. Before adding display consumers, make sure the whole active display
pipeline is represented in genpd and its runtime suspend callbacks have quiesced
all DMA and interrupts. Attaching just one block cannot protect an unmanaged
bootloader pipeline. This is not a tested display handover implementation.

Consumer property migration
---------------------------

The Samsung 4.4 exynos7885.dtsi maps these display nodes to pd_dispaud:

* idma_g0, idma_g1, idma_gf, idma_vg0: lines 1247, 1259, 1270, 1281.
* dsim_0: line 1320.
* decon_f: line 1341.

For each ported consumer, replace ``samsung,power-domain = <&pd_dispaud>`` with
``power-domains = <&pd_dispaud>``. The provider has #power-domain-cells = <0>.
There is no Samsung-specific phandle parsing or manual pm_domain assignment.
The platform bus and standard genpd runtime-PM callbacks perform attachment
and domain power management. Future audio consumers need the same property,
but also require the missing audio policy described below.

Register and secure sequencing
------------------------------

The four dispaud_on, dispaud_save, dispaud_off and dispaud_status arrays are
copied without descriptor changes from Samsung 4.4
``drivers/soc/samsung/cal-if/exynos7885/pmucal_local_exynos7885.h:1-106``.
The save array includes all CMU, SYSREG, DPP, DMA and diagnostic descriptors,
not merely the CMU list. Non-PMU windows are shared MMIO mappings; the PMU uses
the existing syscon regmap, including locked, forced masked writes.

The local sequence engine follows ``pmucal_rae.c:62-256`` and the enable/disable
ordering of ``pmucal_local.c:16-145``:

* Save captures SAVE_RESTORE and diagnostic READ entries only. Plain
  SAVE_RESTORE and READ entries ignore condition columns, exactly as in 4.4;
  only COND_* operations test them. WRITE and WAIT do not run during save.
* Off executes all 13 vendor operations, including the 11 PMU preparation
  writes, configuration clear and status wait.
* On writes configuration 0xf, waits for status 0xf, then restores in table
  order. PLL_CHECK_SKIP skips only the following PLL lock wait if bit 31 is
  clear. The PLL bypass writes remain in their original restore-only positions.
* Diagnostic READ entries are saved but never written on restore.
* Polling is bounded as in 4.4: 1us delays, at most 1001 unsuccessful polls.

Secure peripheral context is saved before local register save/off and restored
after local on/restore, following ``exynos-pd.c:83-107`` and
``exynos-el3_mon.c:302-312``. ``include/linux/smc.h:40`` supplies command
0x82000410. arm_smccc_smc replaces the old wrapper, retains the pre-call DSB SY
from ``arch/arm64/kernel/exynos-smc.S:39``, and passes (command, 0/1,
0x148f0000, 0). Secure errors stop the transition instead of merely logging;
an off-sequence error attempts full on/restore recovery and reports failure.

Deliberate exclusions
---------------------

MFCMSCL, ISP, CAM, G3D and logical subdomains are not selected by this driver,
not described in the new DT include, and have no stub callbacks. Their register
lists, parent relationships and firmware policy need a separate port.

The 4.4 PD_OK_AUD veto (``exynos-pd.c:231-238``), ABOX disable callback, idle-IP
accounting, BTS/devfreq, BCM and Samsung PD debug device are not ported. Their
subsystems are not part of this provider; there are no no-op replacement hooks.
In particular, do not enable ABOX or modem/audio firmware with this experimental
provider until active audio and CP-call power-down constraints are implemented.
The 4.4 CP-call test flag belongs to the unported system suspend/debug machinery
(``exynos-pm.c:405-410``), not a hardware status bit. No substitute status value
is invented. No -EOPNOTSUPP, empty function bodies or TODOs are used here.

Only object compilation and the M20 DTB are validated locally. Actual PMU
transitions, MMIO register retention, audio PLL relock, SMC firmware ABI,
timeouts/recovery, suspend/resume and display handover require hardware testing.
No hardware access, flashing or full Image build is part of this change.
