/*
 * RISC-V Worlds Global Device
 *
 * Copyright (c) 2022 SiFive, Inc.
 *
 * This provides World global config.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "exec/hwaddr.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/riscv_world.h"
#include "hw/core/cpu.h"
#include "target/riscv/cpu.h"
#include "trace.h"

/*
 * World global config:
 * List the global setting of World, like num-of-worlds. It is unique in the machine.
 * All CPUs with World extension and wgChecker devices will use it.
 */
struct RISCVWorldState *world_config;

/* perm field bitmask of wgChecker slot, it's depends on NWorld. */
uint64_t wgc_slot_perm_mask;

static const Property riscv_world_properties[] = {
    DEFINE_PROP_UINT32("nworlds", RISCVWorldState, nworlds, 0),

    /* Only Trusted WID could access wgCheckers if it is enabled. */
    DEFINE_PROP_UINT32("trustedwid", RISCVWorldState, trustedwid, NO_TRUSTEDWID),

    /*
     * World reset value is bypass mode in HW. All World permission checkings are
     * pass by default, so SW could correctly run on the machine w/o any World
     * programming.
     */
    DEFINE_PROP_BOOL("hw-bypass", RISCVWorldState, hw_bypass, false),

    /*
     * TrustZone compatible mode:
     * This mode is only supported in 2 worlds system. It converts World
     * WID to TZ NS signal on the bus so World could be cooperated with
     * TZ components. In QEMU, it converts WID to 'MemTxAttrs.secure' bit used
     * by TZ.
     */
    DEFINE_PROP_BOOL("tz-compat", RISCVWorldState, tz_compat, false),
};

/* WID to MemTxAttrs converter */
void wid_to_mem_attrs(MemTxAttrs *attrs, uint32_t wid)
{
    /* number-of-worlds + 1 (unspecified attrs) */
    g_assert(wid <= world_config->nworlds);

    attrs->unspecified = 0;
    if (world_config->tz_compat) {
        attrs->secure = wid;
    } else {
        attrs->world_id = wid;
    }
}

/* MemTxAttrs to WID converter */
uint32_t mem_attrs_to_wid(MemTxAttrs attrs)
{
    /*
     * Convert the unspecified attribute transaction to
     * QEMU specific WID (nworlds). QEMU loader (-bios, -device loader) can
     * use it to bypass the checks of wgChecker.
     */
    if (attrs.unspecified) {
        return world_config->nworlds;
    }

    if (world_config->tz_compat) {
        return attrs.secure;
    } else {
        return attrs.world_id;
    }
}

static void riscv_cpu_wg_reset(CPURISCVState *env)
{
    CPUState *cs = env_cpu(env);
    RISCVCPU *cpu = RISCV_CPU(cs);
    uint32_t mlwid, slwid, mwiddeleg;
    uint32_t trustedwid;

    if (world_config == NULL) {
        /*
         * Note: This reset is dummy now and World CSRs will be reset again
         * after world_config is realized.
         */
        return;
    }

    trace_riscv_wgcpu_reset(env->mhartid);

    trustedwid = world_config->trustedwid;
    if (trustedwid == NO_TRUSTEDWID) {
        trustedwid = world_config->nworlds - 1;
    }

    /* Reset mlwid, slwid, mwiddeleg CSRs */
    if (world_config->hw_bypass) {
        /* HW bypass mode */
        mlwid = trustedwid;
    } else {
        mlwid = 0;
    }
    slwid = 0;
    mwiddeleg = 0;

    if (riscv_cpu_cfg(env)->ext_smlwid) {
        env->mlwid = mlwid;
    }

    if (riscv_cpu_cfg(env)->ext_sswid) {
        env->slwid = slwid;
    }

    if (riscv_cpu_cfg(env)->ext_smwiddeleg) {
        env->mwiddeleg = mwiddeleg;
    }

    /*
     * Check the platform-defined values (CPU Properties):
     * pmwid, pmwidlist, pmlwidlist
     */
    uint32_t valid_widlist = MAKE_64BIT_MASK(0, world_config->nworlds);

    /* Use default pmwid, pmwidlist, pmlwidlist if CPU properties are not set */
    if (cpu->cfg.pmwid == UINT32_MAX) {
        cpu->cfg.pmwid = trustedwid;
    }

    if (cpu->cfg.pmwidlist == UINT32_MAX) {
        cpu->cfg.pmwidlist = valid_widlist;
    }

    if (cpu->cfg.pmlwidlist == UINT32_MAX) {
        cpu->cfg.pmlwidlist = valid_widlist;
    }

    /* Check if pmwid/pmwidlist/pmlwidlist HW config is valid in NWorld. */
    g_assert(cpu->cfg.pmwid < world_config->nworlds);
    g_assert((cpu->cfg.pmwidlist & ~valid_widlist) == 0);
    g_assert((cpu->cfg.pmlwidlist & ~valid_widlist) == 0);

    /* Reset mwid, mlwidlist CSRs based on platform-defined values. */
    if (riscv_cpu_cfg(env)->ext_smwid) {
        env->mwid = cpu->cfg.pmwid;
    }

    if (riscv_cpu_cfg(env)->ext_smlwidlist) {
        env->mlwidlist = cpu->cfg.pmwidlist;
    }
}

/*
 * riscv_world_apply_cpu - Apply global World resources to CPU.
 *
 * Note: This API should be used after global World device is created
 * (riscv_world_realize()).
 */
void riscv_world_apply_cpu(uint32_t hartid, bool enable_cpu_exts)
{
    CPUState *cpu = cpu_by_arch_id(hartid);
    RISCVCPU *rcpu = RISCV_CPU(cpu);
    CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;

    /* World global config should exist */
    g_assert(world_config);

    /* If the CPU with this hartid doesn't exist */
    if (env == NULL) {
        return;
    }

    /*
     * Enable CPU extensions of RV World automatically.
     *
     * Note: Here is too late to enable World CSRs for GDB. If you'd like to
     * use GDB to access World CSRs, please enable it via CPU options directly.
     */
    if (enable_cpu_exts) {
        rcpu->cfg.ext_smlwid = true;
        rcpu->cfg.ext_smwid = true;
        rcpu->cfg.ext_smlwidlist = true;
        if (riscv_has_ext(env, RVS) && riscv_has_ext(env, RVU)) {
            rcpu->cfg.ext_sswid = true;
            rcpu->cfg.ext_smwiddeleg = true;
        }
    }

    /* Set machine specific callback of RV World */
    env->wg_reset = riscv_cpu_wg_reset;
    env->wid_to_mem_attrs = wid_to_mem_attrs;

    /* Reset World CSRs in CPU */
    env->wg_reset(env);
}

bool could_access_wgblocks(MemTxAttrs attrs, const char *wgblock)
{
    uint32_t wid = mem_attrs_to_wid(attrs);
    uint32_t trustedwid = world_config->trustedwid;

    if ((trustedwid == NO_TRUSTEDWID) || (wid == trustedwid)) {
        return true;
    } else {
        /*
         * Only Trusted WID could access World blocks if having it.
         * Access them from other WIDs will get failed.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Invalid access to %s from non-trusted WID %d\n",
                      __func__, wgblock, wid);

        return false;
    }
}

static void riscv_world_realize(DeviceState *dev, Error **errp)
{
    RISCVWorldState *s = RISCV_WORLD(dev);

    if (world_config != NULL) {
        error_setg(errp, "Couldn't realize multiple global World configs.");
        return;
    }

    if ((s->nworlds) & (s->nworlds - 1)) {
        error_setg(errp, "Current implementation only support power-of-2 NWorld.");
        return;
    }

    if ((s->trustedwid != NO_TRUSTEDWID) && (s->trustedwid >= s->nworlds)) {
        error_setg(errp, "Trusted WID must be less than the number of world.");
        return;
    }

    if ((s->nworlds != 2) && (s->tz_compat)) {
        error_setg(errp, "Only 2 worlds system could use TrustZone compatible mode.");
        return;
    }

    /* Register World global config */
    world_config = s;

    /* Initialize global data for wgChecker */
    wgc_slot_perm_mask = MAKE_64BIT_MASK(0, 2 * world_config->nworlds);
}

static void riscv_world_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, riscv_world_properties);
    dc->user_creatable = true;
    dc->realize = riscv_world_realize;
}

static const TypeInfo riscv_world_info = {
    .name          = TYPE_RISCV_WORLD,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(RISCVWorldState),
    .class_init    = riscv_world_class_init,
};

/*
 * Create World global config
 */
DeviceState *riscv_world_create(uint32_t nworlds, uint32_t trustedwid,
                                 bool hw_bypass, bool tz_compat)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_WORLD);
    qdev_prop_set_uint32(dev, "nworlds", nworlds);
    qdev_prop_set_uint32(dev, "trustedwid", trustedwid);
    qdev_prop_set_bit(dev, "hw-bypass", hw_bypass);
    qdev_prop_set_bit(dev, "tz-compat", tz_compat);
    qdev_realize(DEVICE(dev), NULL, &error_fatal);
    return dev;
}

static void riscv_world_register_types(void)
{
    type_register_static(&riscv_world_info);
}

type_init(riscv_world_register_types)
