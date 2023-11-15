/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include "precomp.h"
#include "gl_fw_log.h"
#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
#include "connv3_debug_utility.h"
#include "connsyslog/connv3_mcu_log.h"
#else
#include "connsys_debug_utility.h"
#endif

#if IS_ENABLED(CFG_SUPPORT_CONNAC1X)
#define CONNLOG_TYPE_WF			CONNLOG_TYPE_WIFI
#elif (CFG_SUPPORT_CONNAC2X == 1)
#define CONNLOG_TYPE_WF			CONN_DEBUG_TYPE_WIFI
#else
#define CONNLOG_TYPE_WF			CONNV3_DEBUG_TYPE_WIFI
#endif
#include "gl_kal.h"
#include "conn_dbg.h"
#include "gl_emi.h"

#define EMI_TX_PWR_OFFSET 0x558000
#define EMI_TX_PWR_BUF_SIZE 128
#define EMI_TX_PWR_BUF_OFFSET 0x20
#define EMI_TX_PWR_KEY1_VAL 0x44332211
#define EMI_TX_PWR_KEY2_VAL 0x88776655
#define EMI_TX_PWR_BUF_SAFETY_MEM_SIZE 5

struct fw_log_wifi_interface fw_log_wifi_inf;

static int fw_log_wifi_open(struct inode *inode, struct file *file);
static int fw_log_wifi_release(struct inode *inode, struct file *file);
static ssize_t fw_log_wifi_read(struct file *filp, char __user *buf,
	size_t len, loff_t *off);
static unsigned int fw_log_wifi_poll(struct file *filp, poll_table *wait);
static long fw_log_wifi_unlocked_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg);
#ifdef CONFIG_COMPAT
static long fw_log_wifi_compat_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg);
#endif
static int emi_tx_power_conn_dbglog(struct mt66xx_chip_info *chip);

const struct file_operations fw_log_wifi_fops = {
	.open = fw_log_wifi_open,
	.release = fw_log_wifi_release,
	.read = fw_log_wifi_read,
	.poll = fw_log_wifi_poll,
	.unlocked_ioctl = fw_log_wifi_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = fw_log_wifi_compat_ioctl,
#endif
};

/* Add abnormal tx power info to conn_dbg_dev */
static int emi_tx_power_conn_dbglog(struct mt66xx_chip_info *chip)
{
	const uint32_t key1 = EMI_TX_PWR_KEY1_VAL, key2 = EMI_TX_PWR_KEY2_VAL;
	uint32_t u4Size = 0, write_idx = 0;
	static uint32_t read_idx;
	char *dbg_msg = NULL;
	/* key1, key2, ringbuf_addr, ringbuf_size, write_idx */
	uint32_t emi_info[5] = {0};

	if (chip == NULL) {
		DBGLOG(INIT, ERROR, "Argument prChipInfo is NULL\n");
		return WLAN_STATUS_FAILURE;
	}
	if (emi_mem_read(chip, EMI_TX_PWR_OFFSET, emi_info, sizeof(emi_info)) != 0) {
		DBGLOG(INIT, ERROR, "EMI memory read failed\n");
		return WLAN_STATUS_FAILURE;
	}

	/* check key value for EMI status */
	if (emi_info[0] != key1 || emi_info[1] != key2) {
		DBGLOG(INIT, ERROR, "EMI not ready. key1=0x%08x, key2=0x%08x\n",
				emi_info[0], emi_info[1]);
		return WLAN_STATUS_FAILURE;
	}
	/* If EMI is reloaded, write_idx will be reset to 0 */
	if (emi_info[4] == 0x0) {
		read_idx = 0x0;
		return WLAN_STATUS_NOT_INDICATING;
	}
	write_idx = emi_info[4];
	if (write_idx > read_idx) {
		u4Size = write_idx - read_idx;
		dbg_msg = kalMemAlloc(u4Size + EMI_TX_PWR_BUF_SAFETY_MEM_SIZE, VIR_MEM_TYPE);
		if (dbg_msg == NULL) {
			DBGLOG(INIT, ERROR, "kalMemAlloc failed\n");
			return WLAN_STATUS_FAILURE;
		}
		kalMemSet(dbg_msg, 0, u4Size + EMI_TX_PWR_BUF_SAFETY_MEM_SIZE);
		emi_mem_read(chip, read_idx + EMI_TX_PWR_OFFSET + EMI_TX_PWR_BUF_OFFSET,
				dbg_msg, u4Size);
		conn_dbg_add_log(CONN_DBG_LOG_TYPE_HW_ERR, dbg_msg);
		read_idx = write_idx;
		kalMemFree(dbg_msg, VIR_MEM_TYPE, u4Size + EMI_TX_PWR_BUF_SAFETY_MEM_SIZE);
		dbg_msg = NULL;
	} else if (write_idx < read_idx) {
		u4Size = emi_info[3] - read_idx + write_idx;
		dbg_msg = kalMemAlloc(u4Size + EMI_TX_PWR_BUF_SAFETY_MEM_SIZE, VIR_MEM_TYPE);
		if (dbg_msg == NULL) {
			DBGLOG(INIT, ERROR, "kalMemAlloc failed\n");
			return WLAN_STATUS_FAILURE;
		}
		kalMemSet(dbg_msg, 0, u4Size);
		emi_mem_read(chip, read_idx + EMI_TX_PWR_OFFSET + EMI_TX_PWR_BUF_OFFSET,
				dbg_msg, emi_info[3] - read_idx);
		emi_mem_read(chip, EMI_TX_PWR_OFFSET + EMI_TX_PWR_BUF_OFFSET,
				&dbg_msg[emi_info[3] - read_idx],
					u4Size + read_idx - emi_info[3]);
		conn_dbg_add_log(CONN_DBG_LOG_TYPE_HW_ERR, dbg_msg);
		read_idx = write_idx;
		kalMemFree(dbg_msg, VIR_MEM_TYPE, u4Size + EMI_TX_PWR_BUF_SAFETY_MEM_SIZE);
		dbg_msg = NULL;
	} else {
		return WLAN_STATUS_NOT_INDICATING;
	}
	return WLAN_STATUS_SUCCESS;
}

void wifi_fwlog_event_func_register(void (*func)(int, int))
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;

	prInf->cb = func;
}

static int fw_log_wifi_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int fw_log_wifi_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t fw_log_wifi_read(struct file *filp, char __user *buf,
	size_t len, loff_t *off)
{
	ssize_t sz = 0;

#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	sz = connv3_log_read_to_user(CONNLOG_TYPE_WF, buf, len);
#else
	sz = connsys_log_read_to_user(CONNLOG_TYPE_WF, buf, len);
#endif

	return sz;
}

static unsigned int fw_log_wifi_poll(struct file *filp, poll_table *wait)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;
	unsigned int ret = 0;

	poll_wait(filp, &prInf->wq, wait);

#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	if (connv3_log_get_buf_size(CONNLOG_TYPE_WF) > 0)
		ret = (POLLIN | POLLRDNORM);
#else
	if (connsys_log_get_buf_size(CONNLOG_TYPE_WF) > 0)
		ret = (POLLIN | POLLRDNORM);
#endif

	return ret;
}

static long fw_log_wifi_unlocked_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;
	int ret = 0;

	down(&prInf->ioctl_mtx);

	switch (cmd) {
	case WIFI_FW_LOG_IOCTL_ON_OFF:{
		int log_on_off = (int)arg;

		if (prInf->cb)
			prInf->cb(WIFI_FW_LOG_CMD_ON_OFF, log_on_off);
		break;
	}
	case WIFI_FW_LOG_IOCTL_SET_LEVEL:{
		int log_level = (int)arg;

		if (prInf->cb)
			prInf->cb(WIFI_FW_LOG_CMD_SET_LEVEL, log_level);

		break;
	}
	case WIFI_FW_LOG_IOCTL_GET_VERSION:{
		prInf->ver_length = 0;
		kalMemZero(prInf->ver_name, MANIFEST_BUFFER_SIZE);
		schedule_work(&prInf->getFwVerQ);
		flush_work(&prInf->getFwVerQ);

		if (copy_to_user((char *) arg, prInf->ver_name,
				 prInf->ver_length))
			ret = -EFAULT;

		DBGLOG(INIT, INFO, "ver_name=%s\n", prInf->ver_name);
		break;
	}
	default:
		ret = -EINVAL;
	}

	up(&prInf->ioctl_mtx);

	return ret;
}

#ifdef CONFIG_COMPAT
static long fw_log_wifi_compat_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	long ret = 0;

	if (!filp->f_op || !filp->f_op->unlocked_ioctl)
		return -ENOTTY;

	fw_log_wifi_unlocked_ioctl(filp, cmd, arg);

	return ret;
}
#endif

static void fw_log_wifi_inf_event_cb(void)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;

	wake_up_interruptible(&prInf->wq);
}

static void fw_log_get_version_workQ(struct work_struct *work)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;
	struct mt66xx_chip_info *prChipInfo;

	glGetChipInfo((void **)&prChipInfo);
	if (prChipInfo && prChipInfo->fw_dl_ops->getFwVerInfo)
		prChipInfo->fw_dl_ops->getFwVerInfo(prInf->ver_name,
			&prInf->ver_length, MANIFEST_BUFFER_SIZE);
}

uint32_t fw_log_notify_rcv(enum ENUM_FW_LOG_CTRL_TYPE type,
	uint8_t *buffer,
	uint32_t size)
{
	uint32_t written = 0;
	struct mt66xx_chip_info *prChipInfo;
#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	enum connv3_log_type eType = CONNV3_LOG_TYPE_PRIMARY;
#endif

#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	switch (type) {
	case ENUM_FW_LOG_CTRL_TYPE_MCU:
		eType = CONNV3_LOG_TYPE_MCU;
		break;
	case ENUM_FW_LOG_CTRL_TYPE_WIFI:
		eType = CONNV3_LOG_TYPE_PRIMARY;
		break;
	default:
		DBGLOG(INIT, ERROR, "invalid type: %d\n",
			type);
		break;
	}
	written = connv3_log_handler(CONNV3_DEBUG_TYPE_WIFI, eType,
		buffer, size);
	if (written == 0)
		DBGLOG(INIT, WARN,
			"[%d] connv3 driver buffer full.\n",
			type);
	else
		DBGLOG(INIT, TRACE,
			"[%d] connv3_log_handler written=%d\n",
			type,
			written);
#endif

	glGetChipInfo((void **)&prChipInfo);
	if (emi_tx_power_conn_dbglog(prChipInfo) == WLAN_STATUS_SUCCESS)
		DBGLOG(INIT, INFO,
		"New abnormal Tx power report is updated in conn_dbg_dev\n");
	return written;
}

int fw_log_wifi_inf_init(void)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;
	int ret = 0;

	INIT_WORK(&prInf->getFwVerQ, fw_log_get_version_workQ);
	init_waitqueue_head(&prInf->wq);
	sema_init(&prInf->ioctl_mtx, 1);

#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	ret = connv3_log_init(CONNLOG_TYPE_WF,
			      RING_BUFFER_SIZE_WF_FW,
			      RING_BUFFER_SIZE_WF_MCU,
			      fw_log_wifi_inf_event_cb);
	if (ret) {
		DBGLOG(INIT, ERROR,
			"connv3_log_init failed, ret: %d\n",
			ret);
		goto return_fn;
	}
#else
	ret = connsys_log_init(CONNLOG_TYPE_WF);
	if (ret) {
		DBGLOG(INIT, ERROR,
			"connsys_log_init failed, ret: %d\n",
			ret);
		goto return_fn;
	}
	connsys_log_register_event_cb(CONNLOG_TYPE_WF,
				      fw_log_wifi_inf_event_cb);
#endif

	ret = alloc_chrdev_region(&prInf->devno, 0, 1,
				  FW_LOG_WIFI_INF_NAME);
	if (ret) {
		DBGLOG(INIT, ERROR,
			"alloc_chrdev_region failed, ret: %d\n",
			ret);
		goto connsys_deinit;
	}

	cdev_init(&prInf->cdev, &fw_log_wifi_fops);
	prInf->cdev.owner = THIS_MODULE;

	ret = cdev_add(&prInf->cdev, prInf->devno, 1);
	if (ret) {
		DBGLOG(INIT, ERROR,
			"cdev_add failed, ret: %d\n",
			ret);
		goto unregister_chrdev_region;
	}

	prInf->driver_class = class_create(THIS_MODULE,
					   FW_LOG_WIFI_INF_NAME);
	if (IS_ERR(prInf->driver_class)) {
		DBGLOG(INIT, ERROR,
			"class_create failed, ret: %d\n",
			ret);
		ret = PTR_ERR(prInf->driver_class);
		goto cdev_del;
	}

	prInf->class_dev = device_create(prInf->driver_class,
					 NULL, prInf->devno,
					 NULL, FW_LOG_WIFI_INF_NAME);
	if (IS_ERR(prInf->class_dev)) {
		ret = PTR_ERR(prInf->class_dev);
		DBGLOG(INIT, ERROR,
			"class_device_create failed, ret: %d\n",
			ret);
		goto class_destroy;
	}

	goto return_fn;

class_destroy:
	class_destroy(prInf->driver_class);
cdev_del:
	cdev_del(&prInf->cdev);
unregister_chrdev_region:
	unregister_chrdev_region(prInf->devno, 1);
connsys_deinit:
#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	connv3_log_deinit(CONNLOG_TYPE_WF);
#else
	connsys_log_register_event_cb(CONNLOG_TYPE_WF, NULL);
	connsys_log_deinit(CONNLOG_TYPE_WF);
#endif
return_fn:
	if (ret)
		DBGLOG(INIT, ERROR, "ret: %d\n",
			ret);

	return ret;
}

void fw_log_wifi_inf_deinit(void)
{
	struct fw_log_wifi_interface *prInf = &fw_log_wifi_inf;

	device_destroy(prInf->driver_class, prInf->devno);
	class_destroy(prInf->driver_class);
	cdev_del(&prInf->cdev);
	unregister_chrdev_region(prInf->devno, 1);

#if IS_ENABLED(CFG_MTK_WIFI_CONNV3_SUPPORT)
	connv3_log_deinit(CONNLOG_TYPE_WF);
#else
	connsys_log_register_event_cb(CONNLOG_TYPE_WF, NULL);
	connsys_log_deinit(CONNLOG_TYPE_WF);
#endif
}

