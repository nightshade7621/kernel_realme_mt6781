/*
 * arch/arm/kernel/topology.c
 *
 * Copyright (C) 2011 Linaro Limited.
 * Written by: Vincent Guittot
 *
 * based on arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/arch_topology.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/topology.h>

/*
 * cpu capacity scale management
 */

/*
 * cpu capacity table
 * This per cpu data structure describes the relative capacity of each core.
 * On a heteregenous system, cores don't have the same computation capacity
 * and we reflect that difference in the cpu_capacity field so the scheduler
 * can take this difference into account during load balance. A per cpu
 * structure is preferred because each CPU updates its own cpu_capacity field
 * during the load balance except for idle cores. One idle core is selected
 * to run the rebalance_domains for all idle cores and the cpu_capacity can be
 * updated during this sequence.
 */

#ifdef CONFIG_OF
struct cpu_efficiency {
	const char *compatible;
	unsigned long efficiency;
};

/*
 * Table of relative efficiency of each processors
 * The efficiency value must fit in 20bit and the final
 * cpu_scale value must be in the range
 *   0 < cpu_scale < 3*SCHED_CAPACITY_SCALE/2
 * in order to return at most 1 when DIV_ROUND_CLOSEST
 * is used to compute the capacity of a CPU.
 * Processors that are not defined in the table,
 * use the default SCHED_CAPACITY_SCALE value for cpu_scale.
 */
static const struct cpu_efficiency table_efficiency[] = {
	{"arm,cortex-a15", 3891},
	{"arm,cortex-a7",  2048},
	{NULL, },
};

static unsigned long *__cpu_capacity;
#define cpu_capacity(cpu)	__cpu_capacity[cpu]

static unsigned long middle_capacity = 1;
static bool cap_from_dt = true;

static int __init get_cpu_for_node(struct device_node *node)
{
	struct device_node *cpu_node;
	int cpu;

	cpu_node = of_parse_phandle(node, "cpu", 0);
	if (!cpu_node)
		return -1;

	for_each_possible_cpu(cpu) {
		if (of_get_cpu_node(cpu, NULL) == cpu_node) {
			topology_parse_cpu_capacity(cpu_node, cpu);
			of_node_put(cpu_node);
			return cpu;
		}
	}

	pr_crit("Unable to find CPU node for %pOF\n", cpu_node);

	of_node_put(cpu_node);
	return -1;
}

static int __init parse_core(struct device_node *core, int cluster_id,
				int core_id)
{
	char name[10];
	bool leaf = true;
	int i = 0;
	int cpu;
	struct device_node *t;

	do {
		snprintf(name, sizeof(name), "thread%d", i);
		t = of_get_child_by_name(core, name);
		if (t) {
			leaf = false;
			cpu = get_cpu_for_node(t);
			if (cpu >= 0) {
				cpu_topology[cpu].socket_id = cluster_id;
				cpu_topology[cpu].core_id = core_id;
				cpu_topology[cpu].thread_id = i;
			} else {
				pr_err("%pOF: Can't get CPU for thread\n",
					t);
				of_node_put(t);
				return -EINVAL;
			}
			of_node_put(t);
		}
		i++;
	} while (t);

	cpu = get_cpu_for_node(core);
	if (cpu >= 0) {
		if (!leaf) {
			pr_err("%pOF: Core has both threads and CPU\n",
			core);
			return -EINVAL;
		}

		cpu_topology[cpu].socket_id = cluster_id;
		cpu_topology[cpu].core_id = core_id;
	} else if (leaf) {
		pr_err("%pOF: Can't get CPU for leaf core\n", core);
		return -EINVAL;
	}

	return 0;
}

static int __init parse_cluster(struct device_node *cluster, int depth)
{
	char name[10];
	bool leaf = true;
	bool has_cores = false;
	struct device_node *c;

	static int cluster_id __initdata;

	int core_id = 0;
	int i, ret;

	i = 0;
	do {
		snprintf(name, sizeof(name), "cluster%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			leaf = false;
			ret = parse_cluster(c, depth + 1);
			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	i = 0;
	do {
		snprintf(name, sizeof(name), "core%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			has_cores = true;

			if (depth == 0) {
				pr_err("%pOF: cpu-map children should be clusters\n",
					c);
				of_node_put(c);
				return -EINVAL;
			}

			if (leaf) {
				ret = parse_core(c, cluster_id, core_id++);
			} else {
				pr_err("%pOF: Non-leaf cluster with core %s\n",
					cluster, name);
				ret = -EINVAL;
			}

			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	if (leaf && !has_cores)
		pr_warn("%pOF: empty cluster\n", cluster);

	if (leaf)
		cluster_id++;

	return 0;
}

/*
 * Iterate all CPUs' descriptor in DT and compute the efficiency
 * (as per table_efficiency). Also calculate a middle efficiency
 * as close as possible to  (max{eff_i} - min{eff_i}) / 2
 * This is later used to scale the cpu_capacity field such that an
 * 'average' CPU is of middle capacity. Also see the comments near
 * table_efficiency[] and update_cpu_capacity().
 */
static void __init parse_dt_topology(void)
{
	const struct cpu_efficiency *cpu_eff;
	struct device_node *cn = NULL, *cn_cpus = NULL;
	struct device_node *map;
	unsigned long min_capacity = ULONG_MAX;
	unsigned long max_capacity = 0;
	unsigned long capacity = 0;
	int ret;
	int cpu = 0;

	pr_info("parse_dt_topology\n");
	 __cpu_capacity = kcalloc(nr_cpu_ids, sizeof(*__cpu_capacity),
				GFP_NOWAIT);

	cn_cpus = of_find_node_by_path("/cpus");
	if (!cn_cpus) {
		pr_err("No CPU information found in DT\n");
		return;
	}

	for_each_possible_cpu(cpu) {
		const u32 *rate;
		int len;

		/* too early to use cpu->of_node */
		cn = of_get_cpu_node(cpu, NULL);
		if (!cn) {
			pr_err("missing device node for CPU %d\n", cpu);
			continue;
		}

		if (topology_parse_cpu_capacity(cn, cpu)) {
			of_node_put(cn);
			continue;
		}

		cap_from_dt = false;

		for (cpu_eff = table_efficiency; cpu_eff->compatible; cpu_eff++)
			if (of_device_is_compatible(cn, cpu_eff->compatible))
				break;

		if (cpu_eff->compatible == NULL)
			continue;

		rate = of_get_property(cn, "clock-frequency", &len);
		if (!rate || len != 4) {
			pr_err("%pOF missing clock-frequency property\n", cn);
			continue;
		}

		capacity = ((be32_to_cpup(rate)) >> 20) * cpu_eff->efficiency;

		/* Save min capacity of the system */
		if (capacity < min_capacity)
			min_capacity = capacity;

		/* Save max capacity of the system */
		if (capacity > max_capacity)
			max_capacity = capacity;

		cpu_capacity(cpu) = capacity;
	}

	/* If min and max capacities are equals, we bypass the update of the
	 * cpu_scale because all CPUs have the same capacity. Otherwise, we
	 * compute a middle_capacity factor that will ensure that the capacity
	 * of an 'average' CPU of the system will be as close as possible to
	 * SCHED_CAPACITY_SCALE, which is the default value, but with the
	 * constraint explained near table_efficiency[].
	 */
	if (4*max_capacity < (3*(max_capacity + min_capacity)))
		middle_capacity = (min_capacity + max_capacity)
				>> (SCHED_CAPACITY_SHIFT+1);
	else
		middle_capacity = ((max_capacity / 3)
				>> (SCHED_CAPACITY_SHIFT-1)) + 1;

	map = of_get_child_by_name(cn_cpus, "cpu-map");
	if (!map)
		goto out;

	ret = parse_cluster(map, 0);
	of_node_put(map);

out:
	if (cap_from_dt)
		topology_normalize_cpu_scale();
}

/*
 * Look for a customed capacity of a CPU in the cpu_capacity table during the
 * boot. The update of all CPUs is in O(n^2) for heteregeneous system but the
 * function returns directly for SMP system.
 */
static void update_cpu_capacity(unsigned int cpu)
{
	if (!cpu_capacity(cpu) || cap_from_dt)
		return;

	topology_set_cpu_scale(cpu, cpu_capacity(cpu) / middle_capacity);

#ifdef CONFIG_DEBUG_KERNEL
	pr_info("CPU%u: update cpu_capacity %lu\n",
		cpu, topology_get_cpu_scale(NULL, cpu));
#endif
}

#else
static inline void parse_dt_topology(void) {}
static inline void update_cpu_capacity(unsigned int cpuid) {}
#endif

 /*
 * cpu topology table
 */
struct cputopo_arm cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

const struct cpumask *cpu_coregroup_mask(int cpu)
{
	return &cpu_topology[cpu].core_sibling;
}

/*
 * The current assumption is that we can power gate each core independently.
 * This will be superseded by DT binding once available.
 */
const struct cpumask *cpu_corepower_mask(int cpu)
{
	return &cpu_topology[cpu].thread_sibling;
}

static void update_siblings_masks(unsigned int cpuid)
{
	struct cputopo_arm *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];
	int cpu;

	/* update core and thread sibling masks */
	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

		if (cpuid_topo->socket_id != cpu_topo->socket_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);

		if (cpuid_topo->core_id != cpu_topo->core_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);
	}
	smp_wmb();
}

/*
 * store_cpu_topology is called at boot when only one cpu is running
 * and with the mutex cpu_hotplug.lock locked, when several cpus have booted,
 * which prevents simultaneous write access to cpu_topology array
 */
void store_cpu_topology(unsigned int cpuid)
{
	update_siblings_masks(cpuid);

	pr_info("CPU%u: thread %d, cpu %d, socket %d\n",
		cpuid, cpu_topology[cpuid].thread_id,
		cpu_topology[cpuid].core_id,
		cpu_topology[cpuid].socket_id);
}

static inline int cpu_corepower_flags(void)
{
	return SD_SHARE_PKG_RESOURCES  | SD_SHARE_POWERDOMAIN;
}

static struct sched_domain_topology_level arm_topology[] = {
#ifdef CONFIG_SCHED_MC
	{ cpu_corepower_mask, cpu_corepower_flags, SD_INIT_NAME(GMC) },
	{ cpu_coregroup_mask, cpu_core_flags, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

/*
 * init_cpu_topology is called at boot when only one cpu is running
 * which prevent simultaneous write access to cpu_topology array
 */
void __init init_cpu_topology(void)
{
	unsigned int cpu;

	/* init core mask and capacity */
	for_each_possible_cpu(cpu) {
		struct cputopo_arm *cpu_topo = &(cpu_topology[cpu]);

		cpu_topo->thread_id = -1;
		cpu_topo->core_id =  -1;
		cpu_topo->socket_id = -1;
		cpumask_clear(&cpu_topo->core_sibling);
		cpumask_clear(&cpu_topo->thread_sibling);
	}
	smp_wmb();

	parse_dt_topology();

	/* Set scheduler topology descriptor */
	set_sched_topology(arm_topology);
}

int topology_nr_clusters(void)
{
	int cpu;
	int nr_clusters = 0;
	int cluster_id, prev_cluster_id = -1;

	for_each_possible_cpu(cpu) {
		cluster_id = topology_physical_package_id(cpu);
		if (cluster_id != prev_cluster_id) {
			nr_clusters++;
			prev_cluster_id = cluster_id;
		}
	}
	return nr_clusters;
}

