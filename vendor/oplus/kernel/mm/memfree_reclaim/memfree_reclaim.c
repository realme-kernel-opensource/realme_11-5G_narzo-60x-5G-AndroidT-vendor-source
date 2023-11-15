// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#include <uapi/linux/sched/types.h>
#include <linux/sched.h>
#include <linux/memory.h>
#include <linux/freezer.h>
#include <linux/swap.h>
#include <linux/cgroup-defs.h>
#include <linux/device.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/vmstat.h>
#include <linux/mmzone.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <linux/memcontrol.h>
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/of.h>
#include <drm/drm_panel.h>
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
#include <linux/msm_drm_notify.h>
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
#include <linux/mtk_panel_ext.h>
#include <linux/mtk_disp_notify.h>
#endif
#if IS_ENABLED(CONFIG_HYBRIDSWAP_ZRAM)
#include "../hybridswap_zram/hybridswap/internal.h"
#endif

#include "memfree_reclaim.h"

#define OPLUS_MEMFREE_CHECK "memfree_check_reclaim"
#define MAX_BUF_LEN 128
#define PAGES_PER_1MB (1 << 8)

#define MEMFREE_EMPTY_ROUND_SKIP_INTERNVAL 1000
#define MEMFREE_MAX_SKIP_INTERVAL 300000
#define page_to_kb(nr) (nr << (PAGE_SHIFT - 10))
#define MEMFREE_RECLAIM_CPUS "0-3"

atomic_t memfree_empty_round_skip_interval = ATOMIC_INIT(MEMFREE_EMPTY_ROUND_SKIP_INTERNVAL);
atomic_t memfree_max_skip_interval = ATOMIC_INIT(MEMFREE_MAX_SKIP_INTERVAL);
atomic_t memfree_cpuload_threshold = ATOMIC_INIT(0);
atomic64_t memfree_threshold_kb = ATOMIC64_INIT(256000);
atomic_t max_reclaim_threshold = ATOMIC_INIT(10);
atomic_t kthread_sleep_ms = ATOMIC_INIT(5000);
atomic_t memfree_kthread_enable = ATOMIC_INIT(0);
atomic_t memfree_empty_round_check_threshold_kb = ATOMIC_INIT(256);
static atomic_t display_off = ATOMIC_LONG_INIT(0);

static struct proc_dir_entry *oplus_memfree_check_info;
static struct task_struct* oplus_memfree_kthread = NULL;
unsigned int memfree_swapd_skip_interval = 0;
static struct cpumask memfree_reclaim_cpumask;
static atomic_t memfree_reclaim_initial = ATOMIC_INIT(0);

bool last_round_is_empty;
bool memfree_last_round_is_empty;
static int memfree_log_level = MF_LOG_WARN;

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY) || IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static struct notifier_block fb_notif;
#elif IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static void *g_panel_cookie;
#endif

#if IS_ENABLED(CONFIG_OPLUS_MEMFREE_JANK) && IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)
extern u32 get_cpu_load(u32 win_cnt, struct cpumask *mask);
#endif

extern unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
		unsigned long nr_pages,
		gfp_t gfp_mask,
		bool may_swap);

extern struct mem_cgroup *get_next_memcg(struct mem_cgroup *prev);
extern void get_next_memcg_break(struct mem_cgroup *memcg);
extern u64 calc_shrink_ratio(pg_data_t *pgdat);
extern bool zram_is_full(void);

int memfree_loglevel(void)
{
	return memfree_log_level;
}

inline unsigned int get_memfree_cpuload_threshold_value(void)
{
	return atomic_read(&memfree_cpuload_threshold);
}

inline unsigned int get_memfree_threshold_kb_value(void)
{
	return atomic64_read(&memfree_threshold_kb);
}

inline unsigned int get_max_reclaim_threshold_value(void)
{
	return atomic_read(&max_reclaim_threshold);
}

inline unsigned int get_kthread_sleep_ms_value(void)
{
	return atomic_read(&kthread_sleep_ms);
}

inline unsigned int get_memfree_kthread_enable_value(void)
{
	return atomic_read(&memfree_kthread_enable);
}

inline unsigned int get_memfree_empty_round_check_threshold_kb_value(void)
{
	return atomic_read(&memfree_empty_round_check_threshold_kb);
}

inline unsigned int get_memfree_max_skip_interval_value(void)
{
	return atomic_read(&memfree_max_skip_interval);
}

inline unsigned int get_memfree_empty_round_skip_interval_value(void)
{
	return atomic_read(&memfree_empty_round_skip_interval);
}

bool high_memfree_is_suitable(void)
{
	unsigned long cur_max_memfree_threshold = get_memfree_threshold_kb_value();
	unsigned long cur_memfree_page = global_zone_page_state(NR_FREE_PAGES);
	unsigned long cur_memfree_kb = cur_memfree_page << (PAGE_SHIFT - 10);
	if (cur_memfree_kb < cur_max_memfree_threshold) {
		return false;
	} else {
		return true;
	}
}

static inline u64 __calc_nr_to_memfree_reclaim(void)
{
	u32 curr_buffers;
	u64 high_buffers;
	u64 max_reclaim_size_value;
	u64 reclaim_size = 0;

	high_buffers = get_memfree_threshold_kb_value() >> 10;
	max_reclaim_size_value = get_max_reclaim_threshold_value();
	curr_buffers = global_zone_page_state(NR_FREE_PAGES) >> 8;
	if (curr_buffers < high_buffers) {
		reclaim_size = high_buffers - curr_buffers;
		reclaim_size = min(reclaim_size, max_reclaim_size_value);
	}

	return reclaim_size * SZ_1M / PAGE_SIZE;
}

#ifdef CONFIG_OPLUS_MEMFREE_JANK
static bool is_cpu_memfree_busy(void)
{
	unsigned int cpuload = 0;
	int i;
	struct cpumask mask;
	unsigned int cpuload_threshold = get_memfree_cpuload_threshold_value();

	cpumask_clear(&mask);

	for (i = 0; i < 4; i++)
		cpumask_set_cpu(i, &mask);

	cpuload = get_cpu_load(1, &mask);
	memfree_log_dbg("cpuload %d\n", cpuload);

	if (cpuload > cpuload_threshold) {
		return true;
	}
	return false;
}
#endif

static unsigned long swapd_memfree_shrink_anon(pg_data_t *pgdat,
		unsigned long nr_to_reclaim)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long nr_reclaimed = 0;
	unsigned long reclaim_memcg_cnt = 0;
	u64 total_can_reclaimed = calc_shrink_ratio(pgdat);
	unsigned long reclaim_cycles;
	bool exit = false;
	unsigned long reclaim_size_per_cycle = PAGES_PER_1MB >> 1;

	if (atomic_read(&display_off)) {
		memfree_log_dbg("display off\n");
		goto out;
	}

	if (unlikely(total_can_reclaimed == 0)) {
		goto out;
	}

	if (total_can_reclaimed < nr_to_reclaim)
		nr_to_reclaim = total_can_reclaimed;

	reclaim_cycles = nr_to_reclaim / reclaim_size_per_cycle;
	while (reclaim_cycles) {
		while ((memcg = get_next_memcg(memcg))) {
			unsigned long memcg_nr_reclaimed, memcg_to_reclaim;
			memcg_hybs_t *hybs;

			if (high_memfree_is_suitable()) {
				get_next_memcg_break(memcg);
				exit = true;
				break;
			}

			hybs = MEMCGRP_ITEM_DATA(memcg);
			if (!hybs->can_reclaimed) {
				continue;
			}

			memcg_to_reclaim = reclaim_size_per_cycle * hybs->can_reclaimed / total_can_reclaimed;
			memcg_nr_reclaimed = try_to_free_mem_cgroup_pages(memcg,
					memcg_to_reclaim, GFP_KERNEL, true);
			reclaim_memcg_cnt++;
			hybs->can_reclaimed -= memcg_nr_reclaimed;
			nr_reclaimed += memcg_nr_reclaimed;
			if (nr_reclaimed >= nr_to_reclaim) {
				get_next_memcg_break(memcg);
				exit = true;
				break;
			}

			if (hybs->can_reclaimed < 0)
				hybs->can_reclaimed = 0;
		}
		if (exit)
			break;

		if (zram_is_full())
			break;
		reclaim_cycles--;
	}

out:
	memfree_log_info("swapd_memfree_shrink_anon total_reclaim %lu nr_to_reclaim %lu from memcg %lu total_can_reclaimed %lu\n",
			page_to_kb(nr_reclaimed), page_to_kb(nr_to_reclaim),
			reclaim_memcg_cnt, page_to_kb(total_can_reclaimed));

	return nr_reclaimed;
}

static void swapd_memfree_shrink_node()
{
	int nid;
	pg_data_t * pgdat;
	unsigned long nr_to_reclaim;
	unsigned long nr_reclaimed = 0;
	unsigned long before_kill_kb;
	unsigned long after_kill_kb;
	const unsigned int increase_rate = 2;

	if (atomic_read(&display_off)) {
		memfree_log_dbg("display off\n");
		return;
	}

	if (zram_is_full()) {
		memfree_log_warn("zram is full\n");
		return;
	}

	if (high_memfree_is_suitable()) {
		memfree_log_dbg("high memfree is suitable\n");
		return;
	}

	before_kill_kb = (global_zone_page_state(NR_FREE_PAGES)) << (PAGE_SHIFT - 10);

	nr_to_reclaim = __calc_nr_to_memfree_reclaim();

	if (!nr_to_reclaim) {
		memfree_log_warn("nr_to_reclaim = 0\n");
		memfree_swapd_skip_interval = get_memfree_empty_round_skip_interval_value();
		return;
	}

	for_each_node(nid) {
		pgdat = NODE_DATA(nid);
		nr_reclaimed = swapd_memfree_shrink_anon(pgdat, nr_to_reclaim);
	}

	if (nr_reclaimed * 4 < get_memfree_empty_round_check_threshold_kb_value()) {
		if (memfree_last_round_is_empty)
			memfree_swapd_skip_interval = min(memfree_swapd_skip_interval *
					increase_rate,
					get_memfree_max_skip_interval_value());
		else
			memfree_swapd_skip_interval =
				get_memfree_empty_round_skip_interval_value();
		memfree_last_round_is_empty = true;
	} else {
		memfree_swapd_skip_interval = 0;
		memfree_last_round_is_empty = false;
	}

	after_kill_kb = (global_zone_page_state(NR_FREE_PAGES)) << (PAGE_SHIFT - 10);
	memfree_log_info("reclaim %lu KB, before_memfree %lu KB, after_memfree %lu KB, skip_interval = %d\n",
			nr_reclaimed * 4, before_kill_kb, after_kill_kb, memfree_swapd_skip_interval);
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
void swapd_memfree_stop(void)
{
	if (oplus_memfree_kthread) {
		memfree_log_err("memfree reclaim stop start!!!\n");
		kthread_stop(oplus_memfree_kthread);
		memfree_log_err("memfree reclaim stop!!!\n");
		oplus_memfree_kthread = NULL;
	}
}

static int memfree_reclaim_update_cpumask(char *buf, struct pglist_data *pgdat)
{
	int retval;
	struct cpumask temp_mask;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!oplus_memfree_kthread) {
		memfree_log_err("oplus_memfree_kthread is NULL!\n");
		return -1;
	}

	cpumask_clear(&temp_mask);
	retval = cpulist_parse(buf, &temp_mask);
	if (retval < 0 || cpumask_empty(&temp_mask)) {
		memfree_log_err("%s are invalid, use default\n", buf);
		goto use_default;
	}

	if (!cpumask_subset(&temp_mask, cpu_present_mask)) {
		memfree_log_err("%s is not subset of cpu_present_mask, use default\n",
				buf);
		goto use_default;
	}

	if (!cpumask_subset(&temp_mask, cpumask)) {
		memfree_log_err("%s is not subset of cpumask, use default\n", buf);
		goto use_default;
	}

	memfree_log_warn("update cpumask %s\n", buf);
	set_cpus_allowed_ptr(oplus_memfree_kthread, &temp_mask);
	cpumask_copy(&memfree_reclaim_cpumask, &temp_mask);
	return 0;

use_default:
	memfree_log_warn("use default cpumask\n");
	set_cpus_allowed_ptr(oplus_memfree_kthread, cpumask);
	cpumask_copy(&memfree_reclaim_cpumask, cpumask);
	return -EINVAL;
}

static int swapd_memfree(void *data)
{
	bool already_sleep = false;
	pg_data_t * pgdat;
	int nid;
	/*do not runnint on super core */
	cpumask_clear(&memfree_reclaim_cpumask);
	for_each_node_state(nid, N_MEMORY) {
		pgdat = NODE_DATA(nid);
		memfree_log_info("update cpumask\n");
		(void)memfree_reclaim_update_cpumask(MEMFREE_RECLAIM_CPUS, pgdat);
		break;
	}

	while (!kthread_should_stop()) {
		bool ret;
		int sleep_ms;

		set_current_state(TASK_INTERRUPTIBLE);
		if (!get_memfree_kthread_enable_value()) {
			memfree_log_warn("memfree_kthread_enable off!!!\n");
			schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT);
		}

		if (atomic_read(&display_off)) {
			memfree_log_dbg("display off\n");
			schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT);
		}
		set_current_state(TASK_RUNNING);
#ifdef CONFIG_OPLUS_MEMFREE_JANK
		if (is_cpu_memfree_busy()) {
			sleep_ms = get_kthread_sleep_ms_value();
			memfree_log_info("cpu busy\n");
			goto sleep1;
		}
#endif
		ret = high_memfree_is_suitable();
		if (!ret) {
			if (memfree_swapd_skip_interval > 0 && !already_sleep) {
				memfree_log_warn("memfree_swapd_skip_interval > 0\n");
				sleep_ms = memfree_swapd_skip_interval;
				already_sleep = true;
				goto sleep;
			} else {
				swapd_memfree_shrink_node();
				already_sleep = false;
				continue;
			}
		} else {
			sleep_ms = get_kthread_sleep_ms_value();
			goto sleep1;
		}
sleep1:
		memfree_last_round_is_empty = false;
		memfree_swapd_skip_interval = 0;
		already_sleep = false;
sleep:
		memfree_log_dbg("sleep %d ms\n", sleep_ms);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(sleep_ms));
	}
	return 0;
}

static int create_swapd_memfree_thread()
{
	struct sched_param param = {
		.sched_priority = DEFAULT_PRIO,
	};

	if (!atomic_read(&memfree_reclaim_initial)) {
		memfree_log_err("memfree reclaim not enabled!!!\n");
		return -EFAULT;
	}

	if (oplus_memfree_kthread != NULL) {
		memfree_log_warn("oplus_memfree_kthread exist!\n");
		return -EEXIST;
	}

	oplus_memfree_kthread = kthread_create(swapd_memfree, NULL, "oplus_memfree_kthread");
	if (IS_ERR(oplus_memfree_kthread)) {
		memfree_log_err("create_swapd_memfree_thread error!!!\n");
		return -EINVAL;
	}
	sched_setscheduler_nocheck(oplus_memfree_kthread, SCHED_NORMAL, &param);
	set_user_nice(oplus_memfree_kthread, PRIO_TO_NICE(param.sched_priority));
	wake_up_process(oplus_memfree_kthread);
	return 0;
}

static ssize_t memfree_empty_round_check_threshold_kb_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_empty_round_check_threshold_kb set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_empty_round_check_threshold_kb is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("memfree_empty_round_check_threshold_kb is invalid 2\n");
		return -EINVAL;
	}

	if (val < 0 || val > INT_MAX) {
		memfree_log_err("memfree_empty_round_check_threshold_kb is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_info("memfree_empty_round_check_threshold_kb is %d\n", val);
	atomic_set(&memfree_empty_round_check_threshold_kb, val);

	return len;
}

static ssize_t memfree_empty_round_check_threshold_kb_read(struct file *file,
						char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_empty_round_check_threshold_kb);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}


static ssize_t memfree_max_skip_interval_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_max_skip_interval set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_max_skip_interval is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("memfree_max_skip_interval is invalid 2\n");
		return -EINVAL;
	}

	if (val < 0 || val > INT_MAX) {
		memfree_log_err("memfree_max_skip_interval is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_warn("memfree_max_skip_interval is %d\n", val);
	atomic_set(&memfree_max_skip_interval, val);

	return len;
}

static ssize_t memfree_max_skip_interval_read(struct file *file,
						char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_max_skip_interval);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static ssize_t memfree_empty_round_skip_interval_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_empty_round_skip_interval set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_empty_round_skip_interval is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		printk("memfree_empty_round_skip_interval is invalid 2\n");
		return -EINVAL;
	}

	if (val < 0 || val > INT_MAX) {
		memfree_log_err("memfree_empty_round_skip_interval is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_warn("memfree_empty_round_skip_interval is %d\n", val);
	atomic_set(&memfree_empty_round_skip_interval, val);

	return len;
}

static ssize_t memfree_empty_round_skip_interval_read(struct file *file,
						char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_empty_round_skip_interval);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}


static ssize_t memfree_kthread_enable_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_kthread_enable set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_kthread_enable is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("memfree_kthread_enable is invalid 2\n");
		return -EINVAL;
	}

	if ((val != 0) && (val != 1)) {
		memfree_log_err("memfree_kthread_enable is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_info("memfree_kthread_enable is %d\n", val);
	atomic_set(&memfree_kthread_enable, val);
	if (val == 0) {
		memfree_log_err("memfree_kthread_enable stop now!!!\n");
	}

	else if (val == 1) {
		if (oplus_memfree_kthread) {
			memfree_log_err("wake up oplus_memfree_kthread\n");
			wake_up_process(oplus_memfree_kthread);
		} else {
			ret = create_swapd_memfree_thread();
		}

		if (ret != 0)
			return ret;
	}

	return len;
}

static ssize_t memfree_kthread_enable_read(struct file *file,
						char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_kthread_enable);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static ssize_t kthread_sleep_ms_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("kthread_sleep_ms set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("kthread_sleep_ms is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("kthread_sleep_ms is invalid 2\n");
		return -EINVAL;
	}

	if (val < 0 || val > INT_MAX) {
		memfree_log_err("kthread_sleep_ms is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_warn("kthread_sleep_ms is %d\n", val);
	atomic_set(&kthread_sleep_ms, val);

	return len;
}

static ssize_t kthread_sleep_ms_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", kthread_sleep_ms);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static ssize_t max_reclaim_threshold_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("max_reclaim_threshold set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("max_reclaim_threshold is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("max_reclaim_threshold is invalid 2\n");
		return -EINVAL;
	}

	if (val < 0 || val > INT_MAX) {
		memfree_log_err("max_reclaim_threshold is invaild 3\n");
		return -EINVAL;
	}

	memfree_log_warn("max_reclaim_threshold is %d\n", val);
	atomic_set(&max_reclaim_threshold, val);

	return len;
}

static ssize_t max_reclaim_threshold_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", max_reclaim_threshold);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}


static ssize_t memfree_threshold_kb_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	unsigned long val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_threshold_kb set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;

	kbuf[len] = '\0';
	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_threshold_kb is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoul(str, 10, &val);
	if (ret) {
		memfree_log_err("memfree_threshold_kb is invalid\n");
		return -EINVAL;
	}

	if (val < 0) {
		memfree_log_err("memfree_threshold_kb is invaild\n");
		return -EINVAL;
	}

	memfree_log_warn("memfree_threshold_kb is %lu\n", val);
	atomic64_set(&memfree_threshold_kb, val);

	return len;
}

static ssize_t memfree_threshold_kb_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%lu\n", memfree_threshold_kb);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static ssize_t cpu_load_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("cpuload set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("cpuload is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("cpu load is invalid\n");
		return -EINVAL;
	}

	if (val < 0 || val > 100) {
		memfree_log_err("cpuload is invaild\n");
		return -EINVAL;
	}

	memfree_log_warn("memfree_cpuload_threshold %d\n", val);
	atomic_set(&memfree_cpuload_threshold, val);

	return len;
}

static ssize_t cpu_load_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_cpuload_threshold);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static ssize_t memfree_loglevel_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	int val;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_loglevel set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_loglevel is invalid\n");
		return -EINVAL;
	}

	ret = kstrtoint(str, 10, &val);
	if (ret) {
		memfree_log_err("memfree_loglevel is invalid\n");
		return -EINVAL;
	}

	if (val < 0) {
		memfree_log_err("memfree_loglevel is invaild\n");
		return -EINVAL;
	}

	memfree_log_warn("memfree_loglevel is %d\n", val);
	memfree_log_level = val;

	return len;
}

static ssize_t memfree_loglevel_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%d\n", memfree_log_level);

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
		return (len < count ? len : count);
}

static ssize_t memfree_reclaim_cpubind_write(struct file *file,
			const char __user *buff, size_t len, loff_t *ppos)
{
	int ret;
	int nid;
	char kbuf[MAX_BUF_LEN] = {'\0'};
	char *str;
	struct pglist_data *pgdat;

	if (len > MAX_BUF_LEN - 1) {
		memfree_log_err("memfree_reclaim_cpubind set too long!!!\n");
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		memfree_log_err("memfree_reclaim_cpubind is invalid\n");
		return -EINVAL;
	}

	memfree_log_info("memfree_reclaim_cpubind is %s\n", str);

	for_each_node_state(nid, N_MEMORY) {
		pgdat = NODE_DATA(nid);

		ret = memfree_reclaim_update_cpumask(str, pgdat);
		break;
	}

	return len;
}

static ssize_t memfree_reclaim_cpubind_read(struct file *file,
			char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[MAX_BUF_LEN] = {'\0'};
	int len;

	len = snprintf(kbuf, MAX_BUF_LEN, "%*pbl\n", cpumask_pr_args(&memfree_reclaim_cpumask));

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
		return (len < count ? len : count);

	return 0;
}

static const struct proc_ops proc_cpu_load_operations = {
	.proc_write = cpu_load_write,
	.proc_read = cpu_load_read,
};

static const struct proc_ops proc_memfree_threshold_kb_operations = {
	.proc_write = memfree_threshold_kb_write,
	.proc_read = memfree_threshold_kb_read,
};

static const struct proc_ops proc_max_reclaim_threshold_operations = {
	.proc_write = max_reclaim_threshold_write,
	.proc_read = max_reclaim_threshold_read,
};

static const struct proc_ops proc_kthread_sleep_ms_operations = {
	.proc_write = kthread_sleep_ms_write,
	.proc_read = kthread_sleep_ms_read,
};

static const struct proc_ops proc_memfree_kthread_enable_operations = {
	.proc_write = memfree_kthread_enable_write,
	.proc_read = memfree_kthread_enable_read,
};

static const struct proc_ops proc_memfree_empty_round_skip_interval_operations = {
	.proc_write = memfree_empty_round_skip_interval_write,
	.proc_read = memfree_empty_round_skip_interval_read,
};

static const struct proc_ops proc_memfree_max_skip_interval_operations = {
	.proc_write = memfree_max_skip_interval_write,
	.proc_read = memfree_max_skip_interval_read,
};

static const struct proc_ops proc_memfree_empty_round_check_threshold_kb_operations = {
	.proc_write = memfree_empty_round_check_threshold_kb_write,
	.proc_read = memfree_empty_round_check_threshold_kb_read,
};

static const struct proc_ops proc_memfree_loglevel_operations = {
	.proc_write = memfree_loglevel_write,
	.proc_read = memfree_loglevel_read,
};

static const struct proc_ops proc_memfree_reclaim_cpubind_operations = {
	.proc_write = memfree_reclaim_cpubind_write,
	.proc_read = memfree_reclaim_cpubind_read,
};

static int oplus_memfree_proc_init()
{
	static struct proc_dir_entry *entry;
	oplus_memfree_check_info = proc_mkdir(OPLUS_MEMFREE_CHECK, NULL);
	if (!oplus_memfree_check_info) {
		memfree_log_err("create memfree_check_info err\n");
		goto err_info;
	}

	entry = proc_create("cpuload", 0666, oplus_memfree_check_info, &proc_cpu_load_operations);
	if (!entry) {
		memfree_log_err("create cpuload err\n");
		goto err_info;
	}

	entry = proc_create("memfree_threshold_kb", 0666, oplus_memfree_check_info, &proc_memfree_threshold_kb_operations);
	if (!entry) {
		memfree_log_err("create memfree_threshold_kb err\n");
		goto err_memfree_threshold;
	}

	entry = proc_create("max_reclaim_threshold_mb", 0666, oplus_memfree_check_info, &proc_max_reclaim_threshold_operations);
	if (!entry) {
		memfree_log_err("create max_reclaim_threshold_mb err\n");
		goto err_max_raclaim;
	}

	entry = proc_create("kthread_sleep_ms", 0666, oplus_memfree_check_info, &proc_kthread_sleep_ms_operations);
	if (!entry) {
		memfree_log_err("create kthread_sleep_ms err\n");
		goto err_kthread_sleep;
	}

	entry = proc_create("memfree_kthread_enable", 0666, oplus_memfree_check_info, &proc_memfree_kthread_enable_operations);
	if (!entry) {
		memfree_log_err("create kthread_sleep_ms err\n");
		goto err_memfree_kthread_enable;
	}

	entry = proc_create("memfree_empty_round_skip_interval", 0666, oplus_memfree_check_info, &proc_memfree_empty_round_skip_interval_operations);
	if (!entry) {
		memfree_log_err("create memfree_empty_round_skip_interval err\n");
		goto err_memfree_empty_round_skip_interval;
	}

	entry = proc_create("memfree_max_skip_interval", 0666, oplus_memfree_check_info, &proc_memfree_max_skip_interval_operations);
	if (!entry) {
		memfree_log_err("create memfree_max_skip_interval err\n");
		goto err_memfree_max_skip_interval;
	}

	entry = proc_create("memfree_empty_round_check_threshold_kb", 0666, oplus_memfree_check_info, &proc_memfree_empty_round_check_threshold_kb_operations);
	if (!entry) {
		memfree_log_err("create memfree_empty_round_check_threshold_kb err\n");
		goto err_memfree_empty_round_check_threshold_kb;
	}

	entry = proc_create("memfree_loglevel", 0666, oplus_memfree_check_info, &proc_memfree_loglevel_operations);
	if (!entry) {
		memfree_log_err("create memfree_loglevel err\n");
		goto err_memfree_loglevel;
	}

	entry = proc_create("memfree_reclaim_cpubind", 0666, oplus_memfree_check_info, &proc_memfree_reclaim_cpubind_operations);
	if (!entry) {
		memfree_log_err("create memfree_reclaim_cpubind err\n");
		goto err_memfree_reclaim_cpubind;
	}
	return 0;

err_memfree_reclaim_cpubind:
	remove_proc_entry("memfree_loglevel", oplus_memfree_check_info);

err_memfree_loglevel:
	remove_proc_entry("memfree_empty_round_check_threshold_kb", oplus_memfree_check_info);

err_memfree_empty_round_check_threshold_kb:
	remove_proc_entry("memfree_max_skip_interval", oplus_memfree_check_info);

err_memfree_max_skip_interval:
	remove_proc_entry("memfree_empty_round_skip_interval", oplus_memfree_check_info);

err_memfree_empty_round_skip_interval:
	remove_proc_entry("memfree_kthread_enable", oplus_memfree_check_info);

err_memfree_kthread_enable:
	remove_proc_entry("kthread_sleep_ms", oplus_memfree_check_info);

err_kthread_sleep:
	remove_proc_entry("max_reclaim_threshold_mb", oplus_memfree_check_info);

err_max_raclaim:
	remove_proc_entry("memfree_threshold_kb", oplus_memfree_check_info);

err_memfree_threshold:
	remove_proc_entry("cpuload", oplus_memfree_check_info);

err_info:
	memfree_log_err("oplus_memfree_proc_init fail\n");
	return -ENOMEM;
}

#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static struct drm_panel *get_active_panel(void)
{
	int i;
	int count;
	struct device_node *panel_node = NULL;
	struct drm_panel *panel = NULL;
	struct device_node *np = NULL;

	np = of_find_node_by_name(NULL, "oplus,dsi-display-dev");
	if (!np) {
		log_err("oplus,dsi-display-dev node missing\n");
		return NULL;
	}

	log_warn("oplus,dsi-display-dev node found\n");
	count = of_count_phandle_with_args(np, "oplus,dsi-panel-primary", NULL);
	if (count <= 0) {
		log_err("oplus,dsi-panel-primary missing\n");
		goto not_found;
	}

	for (i = 0; i < count; i++) {
		panel_node = of_parse_phandle(np, "oplus,dsi-panel-primary", i);
		panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
		if (!IS_ERR(panel)) {
			log_warn("active panel found\n");
			goto found;
		}
	}
not_found:
	panel = NULL;
found:
	of_node_put(np);
	return panel;
}

static void bright_fb_notifier_callback(enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *client_data)
{
	if (!notification) {
		log_info("%s, invalid notify\n", __func__);
		return;
	}

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		atomic_set(&display_off, 1);
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		atomic_set(&display_off, 0);
		memfree_log_err("display on ,wakeup thread");
		if (oplus_memfree_kthread)
			wake_up_process(oplus_memfree_kthread);
		break;
	default:
		break;
	}
}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
static int bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;

	if (evdata && evdata->data) {
		blank = evdata->data;

		if (*blank ==  MSM_DRM_BLANK_POWERDOWN)
			atomic_set(&display_off, 1);

		else if (*blank == MSM_DRM_BLANK_UNBLANK) {
			memfree_log_err("display on ,wakeup thread");
			atomic_set(&display_off, 0);
			if (oplus_memfree_kthread)
				wake_up_process(oplus_memfree_kthread);
		}
	}

	return NOTIFY_OK;
}
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static int mtk_bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	int *blank = (int *)data;

	if (!blank) {
		return 0;
	}

	if (*blank == MTK_DISP_BLANK_POWERDOWN)
		atomic_set(&display_off, 1);

	else if (*blank == MTK_DISP_BLANK_UNBLANK) {
		memfree_log_err("display on ,wakeup thread");
		atomic_set(&display_off, 0);
		if (oplus_memfree_kthread)
			wake_up_process(oplus_memfree_kthread);
	}
	return NOTIFY_OK;
}
#endif

static void register_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	struct drm_panel *active_panel;
	void *cookie = NULL;

	active_panel = get_active_panel();
	if (active_panel)
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_MM, active_panel, bright_fb_notifier_callback, NULL);

	if (active_panel && !IS_ERR(cookie)) {
		memfree_log_info("%s success\n", __func__);
		g_panel_cookie = cookie;
	} else {
		memfree_log_err("%s failed. need fix\n", __func__);
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	fb_notif.notifier_call = bright_fb_notifier_callback;
	if (msm_drm_register_client(&fb_notif))
		memfree_log_err("msm_drm_register_client failed\n");
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	fb_notif.notifier_call = mtk_bright_fb_notifier_callback;
	if (mtk_disp_notifier_register("memfree_reclaim", &fb_notif))
		memfree_log_err("mtk_disp_notifier_register failed\n");
#endif
}

static void unregister_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	if (g_panel_cookie) {
		panel_event_notifier_unregister(g_panel_cookie);
		g_panel_cookie = NULL;
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	msm_drm_unregister_client(&fb_notif);
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	mtk_disp_notifier_unregister(&fb_notif);
#endif
}

static int __init memfree_shrink_init(void)
{
	int ret = 0;

	ret = oplus_memfree_proc_init();
	if (ret != 0) {
		atomic_set(&memfree_reclaim_initial, 0);
		return ret;
	}
	register_panel_event_notifier();
	atomic_set(&memfree_reclaim_initial, 1);

	return 0;
}

static void __exit destory_memfree_reclaim_procfs(void)
{
	if (oplus_memfree_check_info != NULL && atomic_read(&memfree_reclaim_initial)) {
		remove_proc_entry("memfree_reclaim_cpubind", oplus_memfree_check_info);
		remove_proc_entry("memfree_loglevel", oplus_memfree_check_info);
		remove_proc_entry("memfree_empty_round_check_threshold_kb", oplus_memfree_check_info);
		remove_proc_entry("memfree_max_skip_interval", oplus_memfree_check_info);
		remove_proc_entry("memfree_empty_round_skip_interval", oplus_memfree_check_info);
		remove_proc_entry("memfree_kthread_enable", oplus_memfree_check_info);
		remove_proc_entry("kthread_sleep_ms", oplus_memfree_check_info);
		remove_proc_entry("max_reclaim_threshold_mb", oplus_memfree_check_info);
		remove_proc_entry("memfree_threshold_kb", oplus_memfree_check_info);
		remove_proc_entry("cpuload", oplus_memfree_check_info);
	}
}

static void memfree_shrink_exit(void)
{
		unregister_panel_event_notifier();
		swapd_memfree_stop();
		destory_memfree_reclaim_procfs();
}

module_init(memfree_shrink_init);
module_exit(memfree_shrink_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("memfree shrink feature");


