// SPDX-License-Identifier: GPL-2.0
/*
 * gen2kr_p106.c - Kernel helper to unlock PCIe Gen2 (5.0 GT/s) on NVIDIA Pascal
 *                 mining GPUs (P106-100 10de:1c07/1c06, P106-090 10de:1c09, P104-100 10de:1b87)
 *
 * Mechanism:
 *   Attaches a kretprobe on nvidia.ko's nv_pci_probe(). Right when probe returns:
 *     1. [fast]  Sets 0x8C040 SPEED=1 (Pascal encoding: 1 = 5.0 GT/s).
 *                NEVER writes bit0 (COMMIT) to avoid bus drop / ZOMBIE state.
 *     2. [push]  Unlocks PCIe Gen2 capability declaration via BAR0:
 *                  - XP3G overrides (0x8E110/11C/120/12C)
 *                  - 0x88610 (VSEC_HIER: EXP_HEAD.VER 1 -> 2) & 0x8860C (VSEC_DEVICE)
 *                  - 0x2241C (PUNITS.PCI: bit7=PCIE_SPEED, bit0=PCIE_VERSION;
 *                    strictly guarded against poison / non-zero upper 24 bits)
 *                  - 0x8841C (PRIV_MISC_1: bit15=1, bit11=1, clear [14:13]=0, bit12=0;
 *                    falls back to bit13=1 if LNKCAP2 does not latch)
 *                  - 0x8C2C0 (clear DIS_G2), 0x8C040 (SPEED=1), 0x8872C = 6
 *                  - Polls LNKCAP2 until 0x06 (up to ~2s)
 *     3. Sets LNKCTL2.TLS = 5.0 GT/s on both Endpoint and upstream Root Port,
 *        verifies readback, and triggers Link Retrain from the Root Port only.
 *     4. Runs a safe guard workqueue as a fallback net: automatically stops if
 *        LNKCAP2 != 0x06 or after max_fail_retrains consecutive failed retrains.
 *
 * Module parameters:
 *   dev_id=0x1c07   PCI Device ID (default 0x1c07 = P106-100; supports 0x1c06/0x1c09/0x1b87)
 *   probe_only=0    If 1, performs read-only BAR0 fingerprint check & exits without writing
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/file.h>

#define NV_VENDOR       0x10de
#define NV_DEV_DEFAULT  0x1c07   /* P106-100 */

#define OFF_FUSE_GEN23  0x2157C  /* read-only: GEN23_DIS fuse */
#define OFF_FUSE_GEN3   0x21580  /* read-only: GEN3_DIS fuse */
#define OFF_PUNITS_PCI  0x2241C  /* PUNITS.PCI: bit0=PCIE_VERSION, bit7=PCIE_SPEED (live) */
#define OFF_EXP_HEAD    0x88078  /* PCIe cap header mirror: bits[19:16] = VER (1 or 2) */
#define OFF_LNKCAP      0x88084  /* LNKCAP mirror */
#define OFF_LNKSTA      0x88088  /* LNKSTA mirror */
#define OFF_CAP2        0x880A4  /* LINK_CAP2 mirror */
#define OFF_LNKCTL2     0x880A8  /* LNKCTL2 mirror */
#define OFF_CONFIG      0x8841C  /* PRIV_MISC_1 */
#define OFF_VSECDEV     0x8860C  /* VSEC_DEVICE */
#define OFF_VSEC        0x88610  /* VSEC_HIERARCHY */
#define OFF_LTSSM       0x8872C  /* XVE_OVR */
#define OFF_LINKCFG0    0x8C040  /* LINK_CONFIG_0 */
#define OFF_PL_RATE     0x8C1C0  /* PL_LINK_RATE / UNK1C0 */
#define OFF_CYA         0x8C2C0  /* CYA_0, bit2 = DIS_G2 */
#define OFF_XP3G_OVR0   0x8E110
#define OFF_XP3G_OVR3   0x8E11C
#define OFF_XP3G_VAL0   0x8E120
#define OFF_XP3G_VAL3   0x8E12C
#define BAR0_LEN        0x90000

static unsigned short dev_id = NV_DEV_DEFAULT;
module_param(dev_id, ushort, 0444);
MODULE_PARM_DESC(dev_id, "Pascal GPU PCI Device ID (default 0x1c07 = P106-100; also 0x1c06/0x1c09/0x1b87)");

static bool probe_only = false;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "true = read-only BAR0 fingerprint probe (no writes, no kretprobe)");

static int guard_rounds = 0;
module_param(guard_rounds, int, 0644);
MODULE_PARM_DESC(guard_rounds, "Guard iterations (0 = run continuously while Gen2 active, default 0)");

static int max_fail_retrains = 5;
module_param(max_fail_retrains, int, 0644);
MODULE_PARM_DESC(max_fail_retrains, "Max consecutive failed guard retrains before auto-stop (default 5)");

static int retrain_min_ms = 2000;
module_param(retrain_min_ms, int, 0644);
MODULE_PARM_DESC(retrain_min_ms, "Minimum interval between guard retrains in ms (default 2000)");

static int stage_gap_ms = 150;
module_param(stage_gap_ms, int, 0644);
MODULE_PARM_DESC(stage_gap_ms, "Delay between stage0 (0x8872C=6) and stage1 (Retrain) in ms (default 150)");

static int debounce_cnt = 3;
module_param(debounce_cnt, int, 0644);
MODULE_PARM_DESC(debounce_cnt, "Consecutive Gen1 samples before guard retrains (default 3)");

static int guard_period_ms = 200;
module_param(guard_period_ms, int, 0644);
MODULE_PARM_DESC(guard_period_ms, "Guard polling period in ms (default 200)");

static int speed_settle_ms = 800;
module_param(speed_settle_ms, int, 0644);
MODULE_PARM_DESC(speed_settle_ms, "Wait time in ms after restoring SPEED before retraining (default 800)");

static int allow_busy_retrain = 1;
module_param(allow_busy_retrain, int, 0644);
MODULE_PARM_DESC(allow_busy_retrain, "1 = allow guard retrain when GPU is busy, 0 = skip while busy");

static bool guard_enabled = true;
module_param(guard_enabled, bool, 0644);
MODULE_PARM_DESC(guard_enabled, "Enable/disable guard loop (default true)");

static struct kretprobe krp;
static bool krp_registered;
static bool triggered;
static void __iomem *early_bar;

static bool is_poison(u32 v)
{
	return v == 0xFFFFFFFFu || ((v & 0xFFF00000u) == 0xBAD00000u);
}

static bool verify_bar0_fingerprint(struct pci_dev *gpu, void __iomem *bar, bool verbose)
{
	u32 cfg_lcap = 0, cfg_lcap2 = 0;
	u32 bar_exp, bar_lcap, bar_lsta, bar_lcap2, bar_lctl2;
	u32 r_cfg, r_vsecdev, r_vsec, r_ltssm, r_lkcfg, r_plrate, r_cya;
	u32 r_punits, r_f23, r_f3;
	u32 r_ovr0, r_ovr3, r_val0, r_val3;
	bool match;

	if (pcie_capability_read_dword(gpu, PCI_EXP_LNKCAP, &cfg_lcap) ||
	    pcie_capability_read_dword(gpu, PCI_EXP_LNKCAP2, &cfg_lcap2)) {
		pr_err("gen2kr_p106: [probe] failed to read LNKCAP/LNKCAP2 from PCI config\n");
		return false;
	}

	bar_exp   = readl(bar + OFF_EXP_HEAD);
	bar_lcap  = readl(bar + OFF_LNKCAP);
	bar_lsta  = readl(bar + OFF_LNKSTA);
	bar_lcap2 = readl(bar + OFF_CAP2);
	bar_lctl2 = readl(bar + OFF_LNKCTL2);
	r_cfg     = readl(bar + OFF_CONFIG);
	r_vsecdev = readl(bar + OFF_VSECDEV);
	r_vsec    = readl(bar + OFF_VSEC);
	r_ltssm   = readl(bar + OFF_LTSSM);
	r_lkcfg   = readl(bar + OFF_LINKCFG0);
	r_plrate  = readl(bar + OFF_PL_RATE);
	r_cya     = readl(bar + OFF_CYA);
	r_punits  = readl(bar + OFF_PUNITS_PCI);
	r_f23     = readl(bar + OFF_FUSE_GEN23);
	r_f3      = readl(bar + OFF_FUSE_GEN3);
	r_ovr0    = readl(bar + OFF_XP3G_OVR0);
	r_ovr3    = readl(bar + OFF_XP3G_OVR3);
	r_val0    = readl(bar + OFF_XP3G_VAL0);
	r_val3    = readl(bar + OFF_XP3G_VAL3);

	match = (bar_lcap == cfg_lcap) && (bar_lcap2 == cfg_lcap2) && (bar_lcap != 0xFFFFFFFFu);

	if (verbose || !match) {
		pr_info("gen2kr_p106: [probe] --- BAR0 snapshot (dev=10de:%04x) ---\n", dev_id);
		pr_info("gen2kr_p106: [probe] 0x88078 EXP_HEAD      = 0x%08x (VER=%u)\n",
			bar_exp, (bar_exp >> 16) & 0xF);
		pr_info("gen2kr_p106: [probe] 0x88084 LNKCAP_mirror = 0x%08x (cfg=0x%08x)\n", bar_lcap, cfg_lcap);
		pr_info("gen2kr_p106: [probe] 0x88088 LNKSTA_mirror = 0x%08x (CurSpeed=%u CurWidth=x%u)\n",
			bar_lsta, (bar_lsta >> 16) & 0xF, (bar_lsta >> 20) & 0x3F);
		pr_info("gen2kr_p106: [probe] 0x880a4 LNKCAP2_mirror= 0x%08x (cfg=0x%08x)\n", bar_lcap2, cfg_lcap2);
		pr_info("gen2kr_p106: [probe] 0x880a8 LNKCTL2_mirror= 0x%08x (TLS=%u)\n", bar_lctl2, bar_lctl2 & 0xF);
		pr_info("gen2kr_p106: [probe] 0x8841c PRIV_MISC_1   = 0x%08x (b15=%u gate[14:13]=%u)\n",
			r_cfg, (r_cfg >> 15) & 1, (r_cfg >> 13) & 3);
		pr_info("gen2kr_p106: [probe] 0x8860c VSEC_DEVICE   = 0x%08x\n", r_vsecdev);
		pr_info("gen2kr_p106: [probe] 0x88610 VSEC_HIER     = 0x%08x\n", r_vsec);
		pr_info("gen2kr_p106: [probe] 0x8872c XVE_OVR       = 0x%08x\n", r_ltssm);
		pr_info("gen2kr_p106: [probe] 0x2241c PUNITS_PCI    = 0x%08x (VER=%u SPD=%u)\n",
			r_punits, r_punits & 1, (r_punits >> 7) & 1);
		pr_info("gen2kr_p106: [probe] 0x2157c/21580 FUSES   = GEN23_DIS=0x%08x GEN3_DIS=0x%08x\n",
			r_f23, r_f3);
		pr_info("gen2kr_p106: [probe] 0x8c040 LINK_CONFIG_0 = 0x%08x (SPEED=%u COMMIT=%u)\n",
			r_lkcfg, (r_lkcfg >> 18) & 3, r_lkcfg & 1);
		pr_info("gen2kr_p106: [probe] 0x8c1c0 PL_LINK_RATE  = 0x%08x\n", r_plrate);
		pr_info("gen2kr_p106: [probe] 0x8c2c0 CYA_0         = 0x%08x (DIS_G2=%u)\n", r_cya, (r_cya >> 2) & 1);
		pr_info("gen2kr_p106: [probe] 0x8e110/11c/120/12c   = %08x %08x %08x %08x\n",
			r_ovr0, r_ovr3, r_val0, r_val3);
		pr_info("gen2kr_p106: [probe] BAR0_FINGERPRINT=%s\n", match ? "MATCH" : "MISMATCH");
	}
	return match;
}

static struct delayed_work gen2_work;
static int guard_left;
static int pushback_cnt;
static int retrain_cnt;
static int fail_retrains;
static bool bad_read_logged;
static bool pushing;

static unsigned long last_retrain_jiffies;
static unsigned long last_speed_fix_jiffies;
static int retrain_stage;
static unsigned long stage_start_jiffies;
static unsigned long last_attempt_jiffies;
static int low_streak;
static int nvidia_ref_baseline = -1;

static int read_nvidia_refcnt(void)
{
	struct file *f;
	char buf[16] = {0};
	loff_t pos = 0;
	ssize_t n;
	int val;

	f = filp_open("/sys/module/nvidia/refcnt", O_RDONLY, 0);
	if (IS_ERR(f))
		return -1;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return -1;
	if (kstrtoint(buf, 10, &val))
		return -1;
	return val;
}

static bool gpu_busy(void)
{
	int refs = read_nvidia_refcnt();

	if (refs < 0)
		return true;
	if (nvidia_ref_baseline < 0) {
		nvidia_ref_baseline = refs;
		return false;
	}
	return refs > nvidia_ref_baseline;
}

static void guard_fn(struct work_struct *w)
{
	struct pci_dev *gpu;
	void __iomem *bar;
	u32 v, lk;
	u16 st = 0;
	bool low;
	bool stop_guard = false;

	if (guard_left == 0 && guard_rounds > 0)
		return;
	if (guard_left > 0)
		guard_left--;

	gpu = pci_get_device(NV_VENDOR, dev_id, NULL);
	if (!gpu)
		goto next;

	bar = ioremap(pci_resource_start(gpu, 0), BAR0_LEN);
	if (bar) {
		v = readl(bar + OFF_CAP2);
		if (v == 0xFFFFFFFFu)
			goto unmap;

		/* Abort guard immediately if LNKCAP2 does not advertise 5.0 GT/s */
		if ((v & 0x6u) != 0x6u) {
			pr_warn("gen2kr_p106: [guard] LNKCAP2=0x%08x (no 5.0GT/s capability) -> stopping guard\n",
				v);
			stop_guard = true;
			goto unmap;
		}

		/* Double-check: must not be 0xFFFFFFFF and bit0 (COMMIT) must be 0 */
		lk = readl(bar + OFF_LINKCFG0);
		if (lk == 0xFFFFFFFFu) {
			if (!bad_read_logged) {
				pr_warn("gen2kr_p106: [guard] LINKCFG0=0xffffffff, skipping tick\n");
				bad_read_logged = true;
			}
			goto unmap;
		}
		bad_read_logged = false;
		if ((lk & 1u) != 0) {
			pr_warn_ratelimited("gen2kr_p106: [guard] LINKCFG0 COMMIT=1, refusing write (lk=%08x)\n", lk);
			goto unmap;
		}
		if (((lk >> 18) & 0x3) != 1) {
			writel((lk & ~0x000C0000u) | (1u << 18), bar + OFF_LINKCFG0);
			last_speed_fix_jiffies = jiffies;
			pushback_cnt++;
			pr_info_ratelimited("gen2kr_p106: [guard] restored SPEED=1 (count=%d, prev=%08x)\n",
					    pushback_cnt, lk);
		}

		if (pcie_capability_read_word(gpu, PCI_EXP_LNKSTA, &st))
			goto unmap;
		low = (st & PCI_EXP_LNKSTA_CLS) < PCI_EXP_LNKSTA_CLS_5_0GB;

		if (!low) {
			low_streak = 0;
			retrain_stage = 0;
			fail_retrains = 0;
			goto unmap;
		}
		low_streak++;

		if (!guard_enabled)
			goto unmap;

		if (retrain_stage == 1) {
			struct pci_dev *up;
			u16 ctl = 0;

			if (time_before(jiffies, stage_start_jiffies + msecs_to_jiffies(stage_gap_ms)))
				goto unmap;

			up = pci_upstream_bridge(gpu);
			if (up && !pcie_capability_read_word(up, PCI_EXP_LNKCTL, &ctl)) {
				pcie_capability_write_word(up, PCI_EXP_LNKCTL, ctl | PCI_EXP_LNKCTL_RL);
				pr_info("gen2kr_p106: [guard] Root Port Retrain (attempt=%d, LNKSTA=%04x)\n",
					retrain_cnt, st);
			}
			retrain_stage = 0;
			low_streak = 0;
			goto unmap;
		}

		if (max_fail_retrains > 0 && fail_retrains >= max_fail_retrains) {
			pr_warn("gen2kr_p106: [guard] %d consecutive failed retrains (LNKSTA=%04x, CAP2=%08x) -> stopping guard\n",
				fail_retrains, st, v);
			stop_guard = true;
			goto unmap;
		}

		if (low_streak < debounce_cnt)
			goto unmap;
		if (time_before(jiffies, last_attempt_jiffies + msecs_to_jiffies(retrain_min_ms)))
			goto unmap;
		if (time_before(jiffies, last_speed_fix_jiffies + msecs_to_jiffies(speed_settle_ms)))
			goto unmap;
		if (!allow_busy_retrain && gpu_busy()) {
			pr_info_ratelimited("gen2kr_p106: [guard] GPU busy, skipping retrain\n");
			goto unmap;
		}

		writel(0x00000006, bar + OFF_LTSSM);
		retrain_stage = 1;
		stage_start_jiffies = jiffies;
		last_attempt_jiffies = jiffies;
		last_retrain_jiffies = jiffies;
		low_streak = 0;
		retrain_cnt++;
		fail_retrains++;
		pr_info("gen2kr_p106: [guard] trigger 0x8872C=6 (attempt=%d, LNKSTA=%04x)\n",
			retrain_cnt, st);
unmap:
		iounmap(bar);
	}
	pci_dev_put(gpu);

	if (stop_guard)
		return;

next:
	if (guard_rounds > 0 && guard_left <= 0) {
		pr_info("gen2kr_p106: guard finished (speed_fixes=%d, retrains=%d)\n",
			pushback_cnt, retrain_cnt);
		return;
	}
	schedule_delayed_work(&gen2_work, msecs_to_jiffies(guard_period_ms));
}

static void push_fn(struct work_struct *w)
{
	struct pci_dev *gpu;
	struct pci_dev *up;
	void __iomem *bar;
	u32 v, pu, pu_after, cap2, exp_head, cfg_after;
	u16 ctl2, ctl2_rb = 0, ctl, sta = 0;
	int i;
	bool cap2_unlocked = false;

	pushing = false;
	gpu = pci_get_device(NV_VENDOR, dev_id, NULL);
	if (!gpu) {
		pr_warn("gen2kr_p106: GPU 10de:%04x not found\n", dev_id);
		return;
	}

	bar = ioremap(pci_resource_start(gpu, 0), BAR0_LEN);
	if (!bar) {
		pr_err("gen2kr_p106: BAR0 ioremap failed\n");
		goto put;
	}

	pu = readl(bar + OFF_PUNITS_PCI);
	pr_info("gen2kr_p106: [push] start dev=10de:%04x cap2=%08x exp=%08x config=%08x punits=%08x lnkcfg0=%08x\n",
		dev_id, readl(bar + OFF_CAP2), readl(bar + OFF_EXP_HEAD),
		readl(bar + OFF_CONFIG), pu, readl(bar + OFF_LINKCFG0));

	/* 1. Configure XP3G overrides */
	writel(0x00000001, bar + OFF_XP3G_OVR0);
	writel(0x00000004, bar + OFF_XP3G_OVR3);
	writel(0x00000000, bar + OFF_XP3G_VAL0);
	writel(0x00200000, bar + OFF_XP3G_VAL3);

	/* 2. Enable VSEC_HIER (switches EXP_HEAD.VER 1 -> 2) and VSEC_DEVICE */
	v = readl(bar + OFF_VSEC);
	if (!is_poison(v))
		writel((v & ~(1u << 12)) | 1u, bar + OFF_VSEC);
	v = readl(bar + OFF_VSECDEV);
	if (!is_poison(v))
		writel(v | 1u, bar + OFF_VSECDEV);

	/* 3. PUNITS.PCI (0x2241C): live Gen1/Gen2 selector (bit7=FULL, bit0=PCIE v2).
	 *    Only write when upper 24 bits are 0 (matches PUNITS.PCI layout, avoids miswrites).
	 */
	if (!is_poison(pu) && (pu & 0xFFFFFF00u) == 0u) {
		if ((pu & 0x81u) != 0x81u)
			writel(pu | 0x81u, bar + OFF_PUNITS_PCI);
		pu_after = readl(bar + OFF_PUNITS_PCI);
		pr_info("gen2kr_p106: [push] 0x2241C PUNITS.PCI %08x -> %08x\n", pu, pu_after);
	} else {
		pu_after = pu;
		pr_warn("gen2kr_p106: [push] skipping 0x2241C (unexpected/poison value=%08x)\n", pu);
	}

	/* 4. PRIV_MISC_1 (0x8841C): set bit15=1, bit11=1 and clear downgrade gate [14:13]=0, bit12=0
	 *    Condition for LNKCAP2=0x06: LTSSM(0x8872C)=6 && 0x8841C[14:13]=0 && 0x2241C[7]=1
	 */
	v = readl(bar + OFF_CONFIG);
	writel((v | (1u << 11) | (1u << 15)) & ~((1u << 12) | (1u << 13) | (1u << 14)),
	       bar + OFF_CONFIG);

	/* 5. Clear DIS_G2 in CYA_0 */
	v = readl(bar + OFF_CYA);
	if (!is_poison(v))
		writel(v & ~(1u << 2), bar + OFF_CYA);

	/* 6. Set SPEED = 1 (5.0 GT/s), NEVER write bit0 (COMMIT) */
	v = readl(bar + OFF_LINKCFG0);
	if (v == 0xFFFFFFFFu || (v & 1u) != 0) {
		pr_err("gen2kr_p106: [push] untrusted LINKCFG0=%08x, skipping SPEED write\n", v);
	} else {
		writel((v & ~0x000C0000u) | (1u << 18), bar + OFF_LINKCFG0);
	}

	/* 7. Trigger XVE_OVR = 6 and poll LNKCAP2 for 0x06 */
	writel(0x00000006, bar + OFF_LTSSM);

	cap2 = readl(bar + OFF_CAP2);
	for (i = 0; i < 15; i++) {
		if (!is_poison(cap2) && (cap2 & 0x6u) == 0x6u) {
			cap2_unlocked = true;
			break;
		}
		msleep(100);
		if (i == 9)
			writel(0x00000006, bar + OFF_LTSSM);
		cap2 = readl(bar + OFF_CAP2);
	}

	/* Fallback: if [14:13]=0 did not latch CAP2=0x06, also try bit13=1 (0xC0B4AD00 style) */
	if (!cap2_unlocked && !is_poison(cap2)) {
		v = readl(bar + OFF_CONFIG);
		writel((v | (1u << 11) | (1u << 13) | (1u << 15)) & ~((1u << 12) | (1u << 14)),
		       bar + OFF_CONFIG);
		writel(0x00000006, bar + OFF_LTSSM);
		for (i = 0; i < 6; i++) {
			msleep(100);
			cap2 = readl(bar + OFF_CAP2);
			if (!is_poison(cap2) && (cap2 & 0x6u) == 0x6u) {
				cap2_unlocked = true;
				break;
			}
		}
	}

	exp_head = readl(bar + OFF_EXP_HEAD);
	cfg_after = readl(bar + OFF_CONFIG);
	pr_info("gen2kr_p106: [push] post-unlock: cap2=%08x (%s) lnkcap=%08x exp=%08x(VER=%u) config=%08x vsec=%08x/%08x punits=%08x lnkcfg0=%08x plrate=%08x\n",
		cap2, cap2_unlocked ? "UNLOCKED" : "STILL_GEN1",
		readl(bar + OFF_LNKCAP), exp_head, (exp_head >> 16) & 0xF,
		cfg_after, readl(bar + OFF_VSEC), readl(bar + OFF_VSECDEV),
		readl(bar + OFF_PUNITS_PCI), readl(bar + OFF_LINKCFG0),
		readl(bar + OFF_PL_RATE));
	pr_info("gen2kr_p106: [push] xp3g readback = %08x %08x %08x %08x cya=%08x\n",
		readl(bar + OFF_XP3G_OVR0), readl(bar + OFF_XP3G_OVR3),
		readl(bar + OFF_XP3G_VAL0), readl(bar + OFF_XP3G_VAL3),
		readl(bar + OFF_CYA));

	if (!cap2_unlocked)
		pr_warn("gen2kr_p106: [push] LNKCAP2 remained %08x (Gen2 not advertised); trying single Retrain anyway\n",
			cap2);

	/* 8. Set TLS = 5.0 GT/s on Endpoint & Root Port (with readback verification) */
	if (!pcie_capability_read_word(gpu, PCI_EXP_LNKCTL2, &ctl2)) {
		u16 n = (ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT;
		pcie_capability_write_word(gpu, PCI_EXP_LNKCTL2, n);
		pcie_capability_read_word(gpu, PCI_EXP_LNKCTL2, &ctl2_rb);
		pr_info("gen2kr_p106: Endpoint TLS2 %04x -> wrote %04x (readback %04x)\n",
			ctl2, n, ctl2_rb);
	}

	up = pci_upstream_bridge(gpu);
	if (up) {
		if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL2, &ctl2)) {
			u16 n = (ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT;
			pcie_capability_write_word(up, PCI_EXP_LNKCTL2, n);
			pcie_capability_read_word(up, PCI_EXP_LNKCTL2, &ctl2_rb);
			pr_info("gen2kr_p106: RootPort TLS2 %04x -> wrote %04x (readback %04x)\n",
				ctl2, n, ctl2_rb);
		}
		if (!pcie_capability_read_word(gpu, PCI_EXP_LNKSTA, &sta) &&
		    (sta & PCI_EXP_LNKSTA_CLS) >= PCI_EXP_LNKSTA_CLS_5_0GB) {
			pr_info("gen2kr_p106: link already at Gen%u (LNKSTA=%04x), skipping Retrain\n",
				sta & PCI_EXP_LNKSTA_CLS, sta);
		} else if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL, &ctl)) {
			pcie_capability_write_word(up, PCI_EXP_LNKCTL, ctl | PCI_EXP_LNKCTL_RL);
			pr_info("gen2kr_p106: RootPort LNKCTL %04x -> %04x (Retrain)\n",
				ctl, ctl | PCI_EXP_LNKCTL_RL);
		}
	}

	/* 9. Poll LNKSTA for Gen2 */
	for (i = 0; i < 20; i++) {
		msleep(100);
		if (pcie_capability_read_word(gpu, PCI_EXP_LNKSTA, &sta))
			continue;
		if ((sta & PCI_EXP_LNKSTA_CLS) >= PCI_EXP_LNKSTA_CLS_5_0GB) {
			pr_info("gen2kr_p106: SUCCESS Gen%u LNKSTA=%04x (poll %d)\n",
				sta & PCI_EXP_LNKSTA_CLS, sta, i + 1);
			break;
		}
		if (i == 9 && cap2_unlocked && up) {
			writel(0x00000006, bar + OFF_LTSSM);
			msleep(150);
			if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL, &ctl)) {
				pcie_capability_write_word(up, PCI_EXP_LNKCTL, ctl | PCI_EXP_LNKCTL_RL);
				pr_info("gen2kr_p106: [push] second Retrain attempt (LNKSTA=%04x)\n", sta);
			}
		}
	}
	if (i == 20)
		pr_warn("gen2kr_p106: link did not reach Gen2, LNKSTA=%04x (CAP2=%08x)\n",
			sta, readl(bar + OFF_CAP2));

	iounmap(bar);

	/* 10. Start guard only when LNKCAP2 unlocked to Gen2 */
	if (cap2_unlocked) {
		last_retrain_jiffies = jiffies;
		last_attempt_jiffies = jiffies;
		stage_start_jiffies = jiffies;
		retrain_stage = 0;
		low_streak = 0;
		fail_retrains = 0;
		guard_left = (guard_rounds > 0) ? guard_rounds : 1;
		schedule_delayed_work(&gen2_work, msecs_to_jiffies(200));
	} else {
		pr_warn("gen2kr_p106: not starting guard because LNKCAP2 did not unlock\n");
	}

put:
	pci_dev_put(gpu);
}

static DECLARE_WORK(push_work, push_fn);

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	u32 lk;

	if (triggered)
		return 0;
	triggered = true;

	if (early_bar) {
		lk = readl(early_bar + OFF_LINKCFG0);
		if (lk != 0xFFFFFFFFu && (lk & 1u) == 0) {
			writel((lk & ~0x000C0000u) | (1u << 18), early_bar + OFF_LINKCFG0);
			pr_info("gen2kr_p106: [fast] SPEED=1 prev=%08x -> now=%08x\n",
				lk, readl(early_bar + OFF_LINKCFG0));
		} else {
			pr_warn("gen2kr_p106: [fast] LINKCFG0=%08x skipped, deferring to workqueue\n", lk);
		}
	}

	if (!pushing) {
		pushing = true;
		schedule_work(&push_work);
		pr_info("gen2kr_p106: nv_pci_probe returned, queued push_work\n");
	}
	return 0;
}

static int __init gen2kr_p106_init(void)
{
	int ret;
	struct pci_dev *g;
	int rc;
	bool fp_ok;

	memset(&krp, 0, sizeof(krp));
	krp.kp.symbol_name = "nv_pci_probe";
	krp.handler = ret_handler;
	krp.maxactive = 4;
	krp_registered = false;

	INIT_DELAYED_WORK(&gen2_work, guard_fn);

	g = pci_get_device(NV_VENDOR, dev_id, NULL);
	if (!g) {
		pr_err("gen2kr_p106: GPU 10de:%04x not found\n", dev_id);
		return -ENODEV;
	}
	early_bar = ioremap(pci_resource_start(g, 0), BAR0_LEN);
	if (!early_bar) {
		pci_dev_put(g);
		pr_err("gen2kr_p106: BAR0 ioremap failed\n");
		return -ENOMEM;
	}

	/* Always verify read-only BAR0 mirror fingerprint before touching hardware */
	fp_ok = verify_bar0_fingerprint(g, early_bar, probe_only);
	pci_dev_put(g);

	if (probe_only) {
		iounmap(early_bar);
		early_bar = NULL;
		return 0;
	}

	if (!fp_ok) {
		pr_err("gen2kr_p106: BAR0 fingerprint MISMATCH - refusing to load for safety!\n");
		iounmap(early_bar);
		early_bar = NULL;
		return -EINVAL;
	}

	rc = read_nvidia_refcnt();
	if (rc >= 0) {
		nvidia_ref_baseline = rc;
		pr_info("gen2kr_p106: nvidia refcnt baseline = %d\n", rc);
	} else {
		pr_warn("gen2kr_p106: failed to read /sys/module/nvidia/refcnt\n");
	}

	pr_info("gen2kr_p106: pre-mapped BAR0 OK (dev=10de:%04x)\n", dev_id);

	ret = register_kretprobe(&krp);
	if (ret) {
		pr_err("gen2kr_p106: register_kretprobe(nv_pci_probe) failed: %d\n", ret);
		iounmap(early_bar);
		early_bar = NULL;
		return ret;
	}
	krp_registered = true;
	pr_info("gen2kr_p106: attached kretprobe to nv_pci_probe (dev=10de:%04x)\n", dev_id);
	return 0;
}

static void __exit gen2kr_p106_exit(void)
{
	if (krp_registered) {
		unregister_kretprobe(&krp);
		krp_registered = false;
	}
	cancel_work_sync(&push_work);
	cancel_delayed_work_sync(&gen2_work);
	if (early_bar) {
		iounmap(early_bar);
		early_bar = NULL;
	}
	pr_info("gen2kr_p106: module unloaded\n");
}

module_init(gen2kr_p106_init);
module_exit(gen2kr_p106_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unlock PCIe Gen2 (5.0 GT/s) on NVIDIA P106-100 / Pascal mining GPUs");
