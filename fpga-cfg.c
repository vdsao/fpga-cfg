/*
 * FPGA configuration driver for FPP/SPI/CvP/PR.
 *
 * Copyright (C) 2017 DENX Software Engineering
 * Anatolij Gustschin <agust@denx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#if 1
#define DEBUG
#endif

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/uaccess.h>
#include <linux/fsnotify.h>
#include <linux/idr.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 10, 0)
#include <linux/sched.h>
#else
#include <linux/sched/clock.h>
#endif

#define FPGA_DRV_STRING		"fpga_cfg"
#define FPP_RING_MGR_NAME	"ftdi-fpp-fpga-mgr"
#define SPI_RING_MGR_NAME	"altera-ps-spi"
#define SPI_XLNX_MGR_NAME	"Xilinx Slave Serial FPGA Manager"

#define FPGA_CFG_HISTORY_ENTRIES_MIN	500
#define FPGA_CFG_HISTORY_ENTRIES_MAX	10000
#define FPGA_CFG_HISTORY_ENTRIES_DFLT	5000

static unsigned int fpgacfg_hist_len = FPGA_CFG_HISTORY_ENTRIES_DFLT;
module_param(fpgacfg_hist_len, uint, 0);
MODULE_PARM_DESC(fpgacfg_hist_len,
		 "Max number of entries for FPGA config operations history");

static DEFINE_MUTEX(mgr_list_lock);
static struct list_head mgr_devs = LIST_HEAD_INIT(mgr_devs);
static struct dentry *dbgfs_root;

static struct class *fpga_mgr_class;

static DEFINE_IDA(fpga_cfg_ida);

struct fpga_cfg_device {
	struct list_head list;
	struct fpga_manager *mgr;
	struct platform_device *pdev;
};

enum fpga_cfg_mgr_type {
	NOP_MGR,
	FPP_RING_MGR,
	SPI_RING_MGR,
	CVP_MGR,
	PR_MGR,
	FPP_META,
	SPI_META,
	CVP_META,
	PR_META,
	SPI_MGR,
	CFG_BUS_NR,
	CFG_USB_ID,
	CFG_TYPE,
	CFG_BS_LSB,
	FPGA_DRV,
	FPGA_DRV_ARGS,
};

struct fpga_cfg_mgr {
	const char *mgr_name;
	enum fpga_cfg_mgr_type mgr_type;
};

struct fpga_cfg_mgr fpga_cfg_mgr_tbl[] = {
	{ FPP_RING_MGR_NAME, FPP_RING_MGR },
	{ SPI_RING_MGR_NAME, SPI_RING_MGR },
	{ SPI_XLNX_MGR_NAME, SPI_MGR },
	{ },
};

struct cfg_desc {
	struct fpga_manager *mgr;
	struct device *mgr_dev;
	u64 cfg_ts_nsec;
	char firmware[NAME_MAX];
	char metadata[NAME_MAX];
	char firmware_abs[PATH_MAX + NAME_MAX];
	char metadata_abs[PATH_MAX + NAME_MAX];
	char log_tmp[PATH_MAX + NAME_MAX + 64];
};

struct fpga_cfg_log_entry {
	struct list_head list;
	size_t len;
	char entry[];
};

struct fpga_cfg_fpga_inst;

struct fpga_cfg_attribute {
	struct attribute attr;
	ssize_t (*show)(struct fpga_cfg_fpga_inst *,
			struct attribute *, char *);
	ssize_t (*store)(struct fpga_cfg_fpga_inst *, struct attribute *,
			 const char *, size_t);
};

struct fpga_cfg_fpga_inst {
	struct fpga_cfg *cfg;
	size_t idx;
	struct kobject kobj_fpga_dir;
	struct work_struct pci_rm_work;
	wait_queue_head_t wait_queue;

	int bus;
	int dev;
	int func;
	int debug;
	char bdf[16];
	char type[16];
	char usb_dev_id[16];
	char fpga_drv[48];
	char fpga_drv_args[2048];
	int bs_lsb_first;
	enum fpga_cfg_mgr_type mgr_type;
	struct cfg_desc fpp;
	struct cfg_desc spi;
	struct cfg_desc cvp;
	struct cfg_desc pr;

	struct attribute_group pr_attr_grp;
	struct attribute *pr_attrs[3];
	struct fpga_cfg_attribute pr_image_attr;
	struct fpga_cfg_attribute pr_meta_attr;
	size_t pr_seq_num;
	size_t cfg_seq_num;
	char pr_image_attr_name[16];
	char pr_meta_attr_name[16];

	enum fpga_cfg_mgr_type cfg_op1;
	enum fpga_cfg_mgr_type cfg_op2;
	bool cfg_done;

	bool history_header;
	struct mutex history_lock;
	struct list_head history_list;
	size_t history_max_entries;
	size_t history_entries;
	unsigned hist_count;
	unsigned hist_count_new;
	wait_queue_head_t hist_queue;
	struct dentry *dbgfs_history;
};

struct fpga_cfg {
	struct platform_device *pdev;
	struct class *mgr_class;
	struct dentry *dbgfs_devdir;

	struct fpga_cfg_fpga_inst fpga;
};

struct fpga_cfg_platform_data {
	struct fpga_manager *mgr;
	enum fpga_cfg_mgr_type mgr_type;
};

struct modprobe_data {
	char *module_name;
	char *module_args;
	bool remove;
};

static void fpga_cfg_modprobe_cleanup(struct subprocess_info *info)
{
	struct modprobe_data *data = info->data;

	if (data) {
		if (!data->remove)
			kfree(data->module_args);
		kfree(data->module_name);
	}
	kfree(info->argv);
}

#define MAX_MOD_ARGS	32

char modprobe_path[] = "/sbin/modprobe";

static int fpga_cfg_modprobe(char *module_name, int wait,
			     bool remove, char *module_args)
{
	struct subprocess_info *info;
	struct modprobe_data *data;
	char **argv;
	char *p;
	int argc = 0;
	static char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		"TERM=linux",
		NULL
	};


	argv = kmalloc(sizeof(char *[MAX_MOD_ARGS]), GFP_KERNEL);
	if (!argv)
		goto out;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		goto free_argv;

	data->module_name = kstrdup(module_name, GFP_KERNEL);
	if (!data->module_name)
		goto free_data;

	data->remove = remove;

	argv[0] = modprobe_path;
	argv[1] = "-q";

	if (remove) {
		argv[2] = "-r";
		argv[3] = "--";
		argv[4] = data->module_name;
		argv[5] = NULL;
	} else {
		argv[2] = "--";
		argv[3] = data->module_name;
		argv[4] = NULL;

		if (module_args) {
			data->module_args = kstrdup(module_args, GFP_KERNEL);
			if (!data->module_args)
				goto free_module_name;

			argc = 0;
			while ((p = strsep(&data->module_args, ",")) != NULL) {
				if (!*p)
					continue;
				argv[4 + argc] = p;
				argc++;
				if (argc == (MAX_MOD_ARGS - 4)) {
					argv[4 + argc] = NULL;
					break;
				}
			}
		}
	}

	info = call_usermodehelper_setup(modprobe_path, argv, envp,
					 GFP_KERNEL, NULL,
					 fpga_cfg_modprobe_cleanup, data);
	if (!info) {
		if (!remove)
			kfree(data->module_args);
		goto free_module_name;
	}

	return call_usermodehelper_exec(info, wait | UMH_KILLABLE);

free_module_name:
	kfree(data->module_name);
free_data:
	kfree(data);
free_argv:
	kfree(argv);
out:
	return -ENOMEM;
}

static int fpga_cfg_add_new_mgr(struct platform_device *pdev,
				struct fpga_manager *mgr)
{
	struct fpga_cfg_device *cfg;

	/* first, check if this mgr is already listed */
	mutex_lock(&mgr_list_lock);
	list_for_each_entry(cfg, &mgr_devs, list) {
		if (cfg->mgr == mgr) {
			mutex_unlock(&mgr_list_lock);
			return 0;
		}
	}
	mutex_unlock(&mgr_list_lock);

	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg) {
		pr_err("failed to alloc memory\n");
		return -ENOMEM;
	}

	cfg->mgr = mgr;
	cfg->pdev = pdev;
	mutex_lock(&mgr_list_lock);
	list_add_tail(&cfg->list, &mgr_devs);
	mutex_unlock(&mgr_list_lock);
	return 0;
}

static void fpga_cfg_cleanup_mgrs(void)
{
	struct fpga_cfg_device *cfg, *tmp_cfg;

	mutex_lock(&mgr_list_lock);
	list_for_each_entry_safe(cfg, tmp_cfg, &mgr_devs, list) {
		pr_debug("%s: %p\n", __func__, cfg);
		if (cfg->mgr) {
			pr_debug("remove: '%s'\n", cfg->mgr->name);
		}
		list_del(&cfg->list);
		kfree(cfg);
	}
	mutex_unlock(&mgr_list_lock);
}

static int fpga_cfg_create_inst(struct fpga_manager *mgr)
{
	struct fpga_cfg_platform_data pdata;
	struct platform_device *pdev;
	enum fpga_cfg_mgr_type mgr_type;
	const char *name;
	int i, id;
	bool found;
	size_t len;

	mgr_type = NOP_MGR;
	found = false;

	for (i = 0; fpga_cfg_mgr_tbl[i].mgr_name; i++) {
		name = fpga_cfg_mgr_tbl[i].mgr_name;
		len = strlen(name);
		pr_debug("LOOKING for [ %s ] in '%s'\n", mgr->name, fpga_cfg_mgr_tbl[i].mgr_name);
		if (!strncmp(name, mgr->name, len)) {
			mgr_type = fpga_cfg_mgr_tbl[i].mgr_type;
			found = true;
			break;
		}
	}

	if (!found) {
		pr_debug("NO RING MGR found\n");
		/*
		 * No ring fpga manager or no single fpga manager found,
		 * so won't create the configuration interface device
		 */
		return 0;
	}

	id = ida_simple_get(&fpga_cfg_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		return id;
	}

	pdata.mgr = mgr;
	pdata.mgr_type = mgr_type;

	pdev = platform_device_register_data(NULL, "fpga-cfg", id,
					     &pdata, sizeof(pdata));
	if (IS_ERR(pdev)) {
		pr_err("Can't create FPGA config device: %ld\n", PTR_ERR(pdev));
		ida_simple_remove(&fpga_cfg_ida, id);
		return PTR_ERR(pdev);
	}
	fpga_cfg_add_new_mgr(pdev, mgr);
	return 0;
}

static void fpga_cfg_remove_inst(struct fpga_manager *mgr)
{
	struct fpga_cfg_device *cfg, *tmp_cfg;

	pr_debug("%s: ##### remove mgr %s\n", __func__, mgr->name);
	mutex_lock(&mgr_list_lock);
	list_for_each_entry_safe(cfg, tmp_cfg, &mgr_devs, list) {
		if (cfg->mgr == mgr) {
			list_del(&cfg->list);
			if (cfg->mgr)
				pr_debug("remove: '%s'\n", cfg->mgr->name);
			platform_device_unregister(cfg->pdev);
			kfree(cfg);
		}
	}
	mutex_unlock(&mgr_list_lock);
}

static int fpga_cfg_mgr_ncb(struct notifier_block *nb, unsigned long val,
			    void *priv)
{
	struct fpga_manager *mgr = priv;
	int result = NOTIFY_OK;

	if (!mgr)
		return NOTIFY_BAD;

	if (!fpga_mgr_class)
		fpga_mgr_class = mgr->dev.class;

	switch (val) {
	case FPGA_MGR_ADD:
		pr_debug("new mgr %p: %s\n", mgr, mgr ? mgr->name : NULL);
		fpga_cfg_create_inst(mgr);
		break;
	case FPGA_MGR_REMOVE:
		pr_debug("mgr gone %p: %s\n", mgr, mgr ? mgr->name : NULL);
		fpga_cfg_remove_inst(mgr);
		break;
	default:
		WARN_ON(1);
		result = NOTIFY_BAD;
	}
	return result;
}

static struct notifier_block fpga_mgr_notifier = {
	.notifier_call = fpga_cfg_mgr_ncb,
};

static struct pci_dev *fpga_cfg_find_cvp_dev(struct fpga_cfg_fpga_inst *inst)
{
	struct pci_dev *pdev;
	struct device *dev;
	unsigned int devfn;

	dev = &inst->cfg->pdev->dev;

	devfn = PCI_DEVFN(inst->dev, inst->func);

	if (inst->debug)
		dev_dbg(dev, "find bus 0x%x, devfn %d\n", inst->bus, devfn);

	pdev = pci_get_domain_bus_and_slot(0, inst->bus, devfn);
	if (!pdev) {
		dev_err(dev, "Can't find CvP/PR PCIe device '%s'\n", inst->bdf);
		return NULL;
	}

	if (inst->debug)
		dev_dbg(dev, "found CvP device '%s'\n", dev_name(&pdev->dev));

	return pdev;
}

static char fpga_cfg_mgr_name_buf[128];

static int fpga_cfg_attach(struct device *dev, void *data)
{
	struct fpga_manager *mgr = to_fpga_manager(dev);

	/*pr_debug("%s ______________________ s\n", __func__);*/
	/*pr_debug("attach to mgr: '%s'\n", mgr->name);*/
	fpga_cfg_create_inst(mgr);
	/*pr_debug("%s ______________________ e\n\n", __func__);*/
	return 0;
}

static int fpga_cfg_attach_mgrs(struct class *class)
{
	return class_for_each_device(class, NULL, NULL, fpga_cfg_attach);
}

static int fpga_cfg_detach(struct device *dev, void *data)
{
	struct fpga_manager *mgr = to_fpga_manager(dev);

	/*pr_debug("%s ______________________ s\n", __func__);*/
	/*pr_debug("detach from mgr: '%s'\n", mgr->name);*/
	fpga_cfg_remove_inst(mgr);
	/*pr_debug("%s ______________________ e\n\n", __func__);*/
	return 0;
}

static int fpga_cfg_detach_mgrs(struct class *class)
{
	return class_for_each_device(class, NULL, NULL, fpga_cfg_detach);
}

static int fpga_cfg_history_header(struct fpga_cfg_fpga_inst *inst)
{
	struct fpga_cfg_log_entry *log;
	int len = 0, ret = 0;
	char *buf;

	buf = kmalloc(128, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err;
	}

	len += snprintf(buf, 128, "=== Config Log for %s device @ %s ===\n",
			inst->type, inst->bdf);

	log = kmalloc(sizeof(*log) + len + 1, GFP_KERNEL);
	if (log) {
		strncpy(log->entry, buf, len + 1);
		log->len = len;
		mutex_lock(&inst->history_lock);
		list_add_tail(&log->list, &inst->history_list);
		inst->hist_count_new += len;
		inst->history_header = true;
		mutex_unlock(&inst->history_lock);
	} else {
		kfree(buf);
		ret = -ENOMEM;
		goto err;
	}
	kfree(buf);
	return ret;
err:
	dev_warn(&inst->cfg->pdev->dev, "No memory for log entry.\n");
	return ret;
}

static void fpga_cfg_free_log(struct fpga_cfg_fpga_inst *inst)
{
	struct fpga_cfg_log_entry *log, *tmp;

	mutex_lock(&inst->history_lock);
	list_for_each_entry_safe(log, tmp, &inst->history_list, list) {
		list_del(&log->list);
		kfree(log);
	}
	inst->history_header = false;
	inst->hist_count = 0;
	inst->hist_count_new = 0;
	mutex_unlock(&inst->history_lock);
}

static void fpga_cfg_update_hist_attr(struct fpga_cfg_fpga_inst *inst)
{
	struct iattr newattrs = {};

	newattrs.ia_valid = ATTR_SIZE | ATTR_FORCE;
	newattrs.ia_size = inst->hist_count_new;

	inode_lock(d_inode(inst->dbgfs_history));
	notify_change(inst->dbgfs_history, &newattrs, NULL);
	inode_unlock(d_inode(inst->dbgfs_history));
}

static ssize_t fpga_cfg_history_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct fpga_cfg_fpga_inst *inst = file->private_data;
	struct fpga_cfg_log_entry *log, *tmp;
	size_t read_cnt, read_max;
	loff_t line_offs, offs = 0;
	int ret;

	if (!count)
		return 0;
	if (!inst->hist_count_new)
		return 0;
	if (*ppos >= inst->hist_count_new)
		return 0;

	read_cnt = 0;
	read_max = count;

	mutex_lock(&inst->history_lock);

	list_for_each_entry_safe(log, tmp, &inst->history_list, list) {

		if (*ppos >= (offs + log->len)) {
			offs += log->len;
			continue;
		}

		line_offs = *ppos - offs;

		ret = simple_read_from_buffer(buf + read_cnt, read_max,
					      &line_offs, log->entry, log->len);
		if (ret < 0) {
			mutex_unlock(&inst->history_lock);
			if (read_cnt)
				return read_cnt;
			return ret;
		}

		read_max -= ret;
		read_cnt += ret;
		*ppos += ret;
		inst->hist_count += ret;

		if (read_cnt == count)
			break;

		offs += log->len;
	}

	mutex_unlock(&inst->history_lock);

	return read_cnt;
}

static int fpga_cfg_history_open(struct inode *inode, struct file *file)
{
	struct fpga_cfg_fpga_inst *inst;

	if (inode->i_private) {
		file->private_data = inode->i_private;
		inst = inode->i_private;
		inst->hist_count = 0;
		return 0;
	}

	return -ENODEV;
}

static ssize_t fpga_cfg_history_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct fpga_cfg_fpga_inst *inst = file->private_data;
	int ret, val;

	ret = sscanf(buf, "%d", &val);
	if (ret != 1)
		return -EINVAL;

	if (val == 0) {
		fpga_cfg_free_log(inst);
		fpga_cfg_history_header(inst);
		fpga_cfg_update_hist_attr(inst);
	}

	return count;
}

static const struct file_operations dbgfs_history_ops = {
	.open = fpga_cfg_history_open,
	.read = fpga_cfg_history_read,
	.write = fpga_cfg_history_write,
	.llseek = default_llseek,
};

static ssize_t fpga_cfg_attr_show(struct kobject *kobj, struct attribute *attr,
				  char *buf)
{
	struct fpga_cfg_attribute *fpga_cfg_attr;
	struct fpga_cfg_fpga_inst *inst;

	inst = container_of(kobj, struct fpga_cfg_fpga_inst, kobj_fpga_dir);
	fpga_cfg_attr = container_of(attr, struct fpga_cfg_attribute, attr);

	if (!fpga_cfg_attr->show)
		return -ENOENT;

	return fpga_cfg_attr->show(inst, attr, buf);
}

static ssize_t fpga_cfg_attr_store(struct kobject *kobj, struct attribute *attr,
				   const char *buf, size_t size)
{
	struct fpga_cfg_attribute *fpga_cfg_attr;
	struct fpga_cfg_fpga_inst *inst;

	inst = container_of(kobj, struct fpga_cfg_fpga_inst, kobj_fpga_dir);
	fpga_cfg_attr = container_of(attr, struct fpga_cfg_attribute, attr);

	if (!fpga_cfg_attr->store)
		return -ENOENT;

	return fpga_cfg_attr->store(inst, attr, buf, size);
}

static const struct sysfs_ops fpga_cfg_sysfs_ops = {
	.show = fpga_cfg_attr_show,
	.store = fpga_cfg_attr_store,
};

#if 0
static ssize_t show_history(struct fpga_cfg_fpga_inst *inst,
			    struct attribute *attr, char *buf)
{
	struct fpga_cfg_log_entry *log, *tmp;
	int len = 0;

	list_for_each_entry_safe(log, tmp, &inst->history_list, list) {
		/*len += sprintf(buf + len, "%s", log->entry);*/
		pr_debug("%d: %s", len++, log->entry);
	}

	return len;
}
#endif

static ssize_t show_debug(struct fpga_cfg_fpga_inst *inst,
			  struct attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", inst->debug);
}

static ssize_t store_debug(struct fpga_cfg_fpga_inst *inst,
			   struct attribute *attr, const char *buf, size_t size)
{
	sscanf(buf, "%d\n", &inst->debug);
	return size;
}

static ssize_t show_load(struct fpga_cfg_fpga_inst *inst,
			 struct attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", inst->cfg_done);
}

static ssize_t show_ready(struct fpga_cfg_fpga_inst *inst,
			  struct attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", inst->cfg_done);
}

static ssize_t show_status(struct fpga_cfg_fpga_inst *inst,
			   struct attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", inst->cfg_done);
}

/*
 * Helper functions and structures for parsing the config description
 */
#define KEY_SZ	64
#define VAL_SZ	(PATH_MAX + NAME_MAX + 3)

static DEFINE_MUTEX(parser_lock);
static char key_buf[KEY_SZ];
static char val_buf[VAL_SZ];
struct key_type_tbl {
	enum fpga_cfg_mgr_type type;
	const char *key;
	bool done;
};

struct key_type_tbl fpga_cfg_key_tbl[] = {
	{ CFG_USB_ID,	"fpp-usb-dev-id" },
	{ CFG_BUS_NR,	"fpga-pcie-bus-nr" },
	{ CFG_TYPE,	"fpga-type" },
	{ CFG_BS_LSB,	"spi-lsb-first" },
	{ FPP_RING_MGR,	"fpp-image" },
	{ SPI_RING_MGR,	"spi-image" },
	{ CVP_MGR,	"cvp-image" },
	{ PR_MGR,	"part-reconf-image" },
	{ FPP_META,	"fpp-image-meta" },
	{ SPI_META,	"spi-image-meta" },
	{ CVP_META,	"cvp-image-meta" },
	{ PR_META,	"part-reconf-image-meta" },
	{ FPGA_DRV,	"mfd-driver" },
	{ FPGA_DRV_ARGS, "mfd-driver-param" },
};

static void reset_key_tbl_search(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fpga_cfg_key_tbl); i++)
		fpga_cfg_key_tbl[i].done = 0;
}

static int parse_line(const char *line, char *key, char *val)
{
	char fmt_key_val_pfx[] = "%23s = \"%";
	char line_fmt_str[24] = "";
	int ret;

	snprintf(line_fmt_str, sizeof(line_fmt_str), "%s%ds",
		 fmt_key_val_pfx, VAL_SZ);

	ret = sscanf(line, line_fmt_str, key, val);
	/*pr_debug("fmt: '%s' , ret %d\n", line_fmt_str, ret);*/
	if (ret > 0)
		return ret;

	pr_err("Invalid line format.\n");
	return -EINVAL;
}

static int chk_and_terminate_val(char *val)
{
	char *p;

	/* check for trailing '";' and terminate the value string  */
	p = strrchr(val, '"');
	if (!p)
		return 0;

	if (p[1] != ';')
		return 0;

	p[0] = 0;

	return 1;
}

static int assign_values(struct fpga_cfg_fpga_inst *inst,
			 char *key, char *val)
{
	struct device *dev = &inst->cfg->pdev->dev;
	char *dst, *dst_sub;
	int i, ret;
	size_t len;

	for (i = 0; i < ARRAY_SIZE(fpga_cfg_key_tbl); i++) {
		/* do not search for already processed key */
		if (fpga_cfg_key_tbl[i].done)
			continue;
		if (strcmp(fpga_cfg_key_tbl[i].key, key))
			continue;

		fpga_cfg_key_tbl[i].done = 1;

		if (!chk_and_terminate_val(val)) {
			dev_err(dev, "Invalid line end: '%s'\n", val);
			return -EINVAL;
		}

		dst_sub = NULL;

		switch (fpga_cfg_key_tbl[i].type) {
		case FPP_RING_MGR:
			dst_sub = inst->fpp.firmware;
			dst = inst->fpp.firmware_abs;
			len = sizeof(inst->fpp.firmware_abs);
			inst->cfg_op1 = FPP_RING_MGR;
			break;
		case SPI_RING_MGR:
			dst_sub = inst->spi.firmware;
			dst = inst->spi.firmware_abs;
			len = sizeof(inst->spi.firmware_abs);
			inst->cfg_op1 = inst->mgr_type;
			break;
		case CVP_MGR:
			dst_sub = inst->cvp.firmware;
			dst = inst->cvp.firmware_abs;
			len = sizeof(inst->cvp.firmware_abs);
			inst->cfg_op2 = CVP_MGR;
			break;
		case PR_MGR:
			dst_sub = inst->pr.firmware;
			dst = inst->pr.firmware_abs;
			len = sizeof(inst->pr.firmware_abs);
			inst->cfg_op1 = PR_MGR;
			break;
		case FPP_META:
			dst = inst->fpp.metadata_abs;
			len = sizeof(inst->fpp.metadata_abs);
			break;
		case SPI_META:
			dst = inst->spi.metadata_abs;
			len = sizeof(inst->spi.metadata_abs);
			break;
		case CVP_META:
			dst = inst->cvp.metadata_abs;
			len = sizeof(inst->cvp.metadata_abs);
			break;
		case PR_META:
			dst = inst->pr.metadata_abs;
			len = sizeof(inst->pr.metadata_abs);
			break;
		case CFG_BUS_NR:
			if (sscanf(val, "%x:%x.%x",
			    &inst->bus, &inst->dev, &inst->func) != 3) {
				dev_err(dev, "Invalid bus-nr: '%s'\n", val);
				return -EINVAL;
			}
			sscanf(val, "%s", inst->bdf);
			if (inst->debug)
				dev_dbg(dev, "BDF '%s'\n", inst->bdf);
			return 0;
		case CFG_USB_ID:
			if (strncmp(val, inst->usb_dev_id,
				    sizeof(inst->usb_dev_id))) {
				dev_warn(dev, "FPP usb id '%s', expected '%s'\n",
					 val, inst->usb_dev_id);
			}
			if (inst->debug)
				dev_dbg(dev, "Using FPP dev '%s'\n", val);
			return 0;
		case CFG_TYPE:
			if (sscanf(val, "%s", inst->type) != 1) {
				dev_err(dev, "Invalid type '%s'\n", val);
				return -EINVAL;
			}
			if (inst->debug)
				dev_dbg(dev, "TYPE '%s'\n", inst->type);
			return 0;
		case CFG_BS_LSB:
			if (sscanf(val, "%d", &inst->bs_lsb_first) != 1) {
				dev_err(dev, "Invalid bitorder flag '%s'\n", val);
				return -EINVAL;
			}
			if (inst->debug) {
				dev_dbg(dev, "Bitstream LSB first flag '%d'\n",
					inst->bs_lsb_first);
			}
			return 0;
		case FPGA_DRV:
			if (sscanf(val, "%s", inst->fpga_drv) != 1) {
				dev_err(dev, "Invalid 'mfd-driver': '%s'\n", val);
				return -EINVAL;
			}
			if (inst->debug)
				dev_dbg(dev, "Using mfd-driver: '%s'\n", inst->fpga_drv);
			return 0;
		case FPGA_DRV_ARGS:
			strncpy(inst->fpga_drv_args, val,
				sizeof(inst->fpga_drv_args));
			if (inst->debug)
				dev_dbg(dev, "Using mfd-driver-param: '%s'\n",
					inst->fpga_drv_args);
			return 0;
		default:
			return 0;
		}
		strncpy(dst, val, len);
		if (inst->debug)
			dev_dbg(dev, "abs. name '%s'\n", dst);
		if (dst_sub) {
			ret = sscanf(val, "/lib/firmware/%s", dst_sub);
			if (ret == 1) {
				if (inst->debug)
					pr_debug("base name '%s'\n", dst_sub);
				return 0;
			}
			dev_err(dev,
				"/lib/firmware/ prefix expected, found: %s\n",
				val);
			return -EINVAL;
		}
	}
	return 0;
}

static int fpga_cfg_desc_parse(struct fpga_cfg_fpga_inst *inst,
			       const char *buf, size_t size)
{
	struct fpga_cfg *cfg = inst->cfg;
	struct device *dev = &cfg->pdev->dev;
	const char *p, *line, *end;
	int ret;

	/* process data buffer until trailing "\n}\n" */
	p = buf;
	end = buf + size - 3;

	mutex_lock(&parser_lock);
	reset_key_tbl_search();

	while (p < end) {
		if (*p == '\n') {
			line = p + 1;
			ret = parse_line(line, key_buf, val_buf);
			if (ret <= 1) {
				dev_err(dev, "parse error: '%s'\n", line);
				mutex_unlock(&parser_lock);
				return -EINVAL;
			}
			ret = assign_values(inst, key_buf, val_buf);
			if (ret < 0) {
				mutex_unlock(&parser_lock);
				return ret;
			}
		}
		p++;
	}
	mutex_unlock(&parser_lock);
	return 0;
}

static int fpga_cfg_op_log(struct fpga_cfg_fpga_inst *inst,
			   struct cfg_desc *desc)
{
	struct fpga_cfg_log_entry *log, *hdr, *first;
	unsigned long rem_nsec;
	int len, ret = 0;

	desc->cfg_ts_nsec = local_clock();
	rem_nsec = do_div(desc->cfg_ts_nsec, 1000000000);
	len = snprintf(desc->log_tmp, sizeof(desc->log_tmp),
		       "[%5lu.%06lu] load %zi: %s\tmeta: %s\n",
			(unsigned long)desc->cfg_ts_nsec,
			rem_nsec / 1000, inst->cfg_seq_num,
			desc->firmware_abs, desc->metadata_abs);

	log = kmalloc(sizeof(*log) + len + 1, GFP_KERNEL);
	if (log) {
		strncpy(log->entry, desc->log_tmp, len + 1);
		log->len = len;
		mutex_lock(&inst->history_lock);
		list_add_tail(&log->list, &inst->history_list);
		inst->history_entries++;
		mutex_unlock(&inst->history_lock);
	} else {
		dev_warn(&inst->cfg->pdev->dev, "No memory for log entry.\n");
		return -ENOMEM;
	}

	if (inst->history_entries > inst->history_max_entries) {
		mutex_lock(&inst->history_lock);
		hdr = list_first_entry(&inst->history_list,
				       struct fpga_cfg_log_entry,
				       list);
		if (hdr) {
			first = list_next_entry(hdr, list);
			inst->hist_count_new -= first->len;
			inst->history_entries--;
			pr_debug("hist. len %zi, delete entry '%s'\n",
				 inst->history_entries, first->entry);
			list_del(&first->list);
			kfree(first);
		}
		mutex_unlock(&inst->history_lock);
	}

	inst->hist_count_new += len;
	fpga_cfg_update_hist_attr(inst);

	return ret;
}

static void bus_rescan_full(void)
{
	struct pci_bus *b = NULL;

	pci_lock_rescan_remove();
	while ((b = pci_find_next_bus(b)) != NULL)
		pci_rescan_bus(b);
	pci_unlock_rescan_remove();
}

static void bus_rescan(struct fpga_cfg_fpga_inst *inst,
		       struct pci_bus *bus, char *driver)
{
	struct pci_dev *pdev;
	unsigned int max;

	pci_lock_rescan_remove();
	max = pci_scan_child_bus(bus);
	pci_assign_unassigned_bus_resources(bus);

	list_for_each_entry(pdev, &bus->devices, bus_list) {
		/* Skip already-added devices */
		if (pdev->is_added)
			continue;

		if (inst->debug)
			pr_debug("%s: add dev %s, drv. override %p\n", __func__,
				 dev_name(&pdev->dev), pdev->driver_override);
		if (driver) {
			if (pdev->driver_override)
				kfree(pdev->driver_override);

			pdev->driver_override = kstrdup(driver, GFP_KERNEL);
		}
		pci_bus_add_device(pdev);
	}
	pci_unlock_rescan_remove();
}

static ssize_t store_load(struct fpga_cfg_fpga_inst *inst,
			  struct attribute *attr,
			  const char *buf, size_t size)
{
	struct fpga_manager *mgr;
	struct cfg_desc *desc;
	struct pci_dev *pdev;
	struct device *dev;
	const char *start, *end;
	struct fpga_image_info info;
	int ret;

	if (!size || size > SZ_16K)
		return -EINVAL;

	if (!inst->cfg) {
		if (inst->debug)
			pr_debug("No cfg device\n");
		return -ENODEV;
	}

	dev = &inst->cfg->pdev->dev;

	start = buf;
	end = buf + size - 3;

	if (strncmp(start, "{\n", 2) || strncmp(end, "\n}\n", 3)) {
		dev_err(dev, "Invalid firmware description.\n");
		if (inst->debug)
			dev_dbg(dev, "'%s', size %zd\n", start, size);
		return -EINVAL;
	}

	memset(&info, 0, sizeof(info));
	inst->bs_lsb_first = 0;
	inst->cfg_done = false;
	inst->cfg_op1 = NOP_MGR;
	inst->cfg_op2 = NOP_MGR;

	ret = fpga_cfg_desc_parse(inst, buf, size);
	if (ret < 0)
		return ret;

	if (inst->debug)
		dev_dbg(dev, "MGRs: %p %p %p %p\n",
			inst->fpp.mgr, inst->spi.mgr,
			inst->cvp.mgr, inst->pr.mgr);

	if (!inst->history_header) {
		fpga_cfg_history_header(inst);
		fpga_cfg_update_hist_attr(inst);
	}

	if (inst->cfg_op1 == FPP_RING_MGR || inst->cfg_op1 == SPI_RING_MGR) {

		if (inst->fpp.mgr) {
			desc = &inst->fpp;
		} else if (inst->spi.mgr) {
			desc = &inst->spi;
		} else
			return -ENODEV;

		if (inst->debug)
			dev_dbg(dev, "FPP/SPI step start\n");

		pdev = fpga_cfg_find_cvp_dev(inst);
		if (pdev) {
			/* Unbind and remove the PCIe FPGA device first */
			pci_lock_rescan_remove();
			pci_dev_get(pdev);
			pci_stop_and_remove_bus_device(pdev);
			pci_dev_put(pdev);
			pci_unlock_rescan_remove();
		}

		ret = fpga_cfg_modprobe(inst->fpga_drv, 1, true, NULL);
		if (ret < 0)
			dev_warn(dev, "Failed to unload module '%s'\n",
				 inst->fpga_drv);

		msleep(10);

		if (inst->cfg_op1 == SPI_RING_MGR) {
			if (inst->bs_lsb_first)
				info.flags = FPGA_MGR_BITSTREAM_LSB_FIRST;
			else
				info.flags &= ~FPGA_MGR_BITSTREAM_LSB_FIRST;
		}

		/* Load ring image now */
		ret = fpga_mgr_firmware_load(desc->mgr, &info, desc->firmware);
		if (ret < 0) {
			dev_warn(dev, "%s fpga_mgr failed: %d\n",
				 inst->fpp.mgr ? "FPP" : "SPI", ret);
			goto err;
		}

		inst->cfg_done = true;
		inst->cfg_seq_num += 1;
		fpga_cfg_op_log(inst, desc);
		if (inst->debug)
			dev_dbg(dev, "FPP/SPI step done\n");
	}

	if (inst->cfg_op1 == SPI_MGR) {
		desc = &inst->spi;
		ret = fpga_mgr_firmware_load(desc->mgr, &info, desc->firmware);
		if (ret < 0) {
			dev_warn(dev, "SPI fpga_mgr failed: %d\n", ret);
			goto err;
		}

		inst->cfg_done = true;
		inst->cfg_seq_num += 1;
		fpga_cfg_op_log(inst, desc);
		if (inst->debug)
			dev_dbg(dev, "SPI cfg. step done\n");
		sysfs_notify(&inst->kobj_fpga_dir, NULL, "status");
		return size;
	}

	if (inst->cfg_op1 == PR_MGR) {
		if (inst->debug)
			dev_dbg(dev, "PR step start\n");
		pdev = fpga_cfg_find_cvp_dev(inst);
		if (!pdev) {
			return -ENODEV;
		}

		inst->pr.mgr = fpga_mgr_get(&pdev->dev);
		if (IS_ERR(inst->pr.mgr)) {
			ret = PTR_ERR(inst->pr.mgr);
			inst->pr.mgr = NULL;
			dev_err(dev, "failed getting PR manager: %d\n", ret);
			return ret;
		}
		if (inst->debug)
			dev_dbg(dev, "Using PR manager: '%s'\n",
				inst->pr.mgr->name);

		info.flags = FPGA_MGR_PARTIAL_RECONFIG;
		ret = fpga_mgr_firmware_load(inst->pr.mgr, &info,
					     inst->pr.firmware);
		if (ret < 0) {
			fpga_mgr_put(inst->pr.mgr);
			inst->pr.mgr = NULL;
			return ret;
		}
		inst->cfg_seq_num += 1;
		fpga_cfg_op_log(inst, &inst->pr);
		fpga_mgr_put(inst->pr.mgr);
		inst->pr_seq_num += 1;
		inst->pr.mgr = NULL;
		inst->cfg_done = true;
		if (inst->debug)
			dev_dbg(dev, "PR step done\n");

		sysfs_remove_file_from_group(&inst->kobj_fpga_dir,
					&inst->pr_image_attr.attr, "pr");
		sysfs_remove_file_from_group(&inst->kobj_fpga_dir,
					&inst->pr_meta_attr.attr, "pr");

		snprintf(&inst->pr_image_attr_name[5],
			 sizeof(inst->pr_image_attr_name) - 5,
			 "%zi", inst->pr_seq_num);
		snprintf(&inst->pr_meta_attr_name[4],
			 sizeof(inst->pr_meta_attr_name) - 4,
			 "%zi", inst->pr_seq_num);

		ret = sysfs_add_file_to_group(&inst->kobj_fpga_dir,
				&inst->pr_image_attr.attr, "pr");
		ret |= sysfs_add_file_to_group(&inst->kobj_fpga_dir,
				&inst->pr_meta_attr.attr, "pr");
		if (ret) {
			dev_err(&pdev->dev, "cannot add pr img file\n");
		}
		sysfs_notify(&inst->kobj_fpga_dir, NULL, "ready");
		return size;
	}

	if (inst->debug)
		dev_dbg(dev, "reattach CvP device...\n");

	pdev = fpga_cfg_find_cvp_dev(inst);
	if (pdev) {
		/* Unbind and remove the PCIe FPGA device first */
		pci_lock_rescan_remove();
		pci_dev_get(pdev);
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
		pci_unlock_rescan_remove();
	}
	msleep(300);

	if (inst->debug)
		dev_dbg(dev, "rescan 1\n");

	if (pdev)
		bus_rescan(inst, pdev->bus, "altera-cvp");
	else
		bus_rescan_full();

	msleep(50);

	if (inst->debug)
		dev_dbg(dev, "rescan 1 done\n");

	if (inst->cfg_op2 == CVP_MGR) {
		inst->cfg_done = false;
		msleep(200);
		pdev = fpga_cfg_find_cvp_dev(inst);
		if (!pdev) {
			dev_dbg(dev, "PCIe FPGA dev not found.\n");
			ret = -ENODEV;
			goto err;
		}

		inst->cvp.mgr_dev = &pdev->dev;

		mgr = fpga_mgr_get(&pdev->dev);
		if (IS_ERR(mgr)) {
			ret = PTR_ERR(mgr);
			dev_err(dev, "failed getting CvP manager: %d\n", ret);
			return ret;
		}
		if (inst->debug) {
			dev_info(dev, "CvP step start\n");
			dev_info(dev, "Using CvP manager: '%s'\n", mgr->name);
		}

		inst->cvp.mgr = mgr;
		ret = fpga_mgr_firmware_load(inst->cvp.mgr, &info,
					     inst->cvp.firmware);
		if (ret < 0) {
			fpga_mgr_put(mgr);
			inst->cvp.mgr = NULL;
			goto err;
		}
		inst->cfg_seq_num += 1;
		inst->cfg_done = true;
		fpga_cfg_op_log(inst, &inst->cvp);
		fpga_mgr_put(mgr);
		inst->cvp.mgr = NULL;
		if (inst->debug)
			dev_info(dev, "CvP step done\n");
	}

	if (inst->debug)
		dev_dbg(dev, "reattach CvP device\n");

	pdev = fpga_cfg_find_cvp_dev(inst);
	if (pdev) {
		/* Unbind and remove the PCIe FPGA device first */
		pci_lock_rescan_remove();
		pci_dev_get(pdev);
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
		pci_unlock_rescan_remove();
	}

	msleep(200);

	ret = fpga_cfg_modprobe(inst->fpga_drv, 1, false, inst->fpga_drv_args);
	if (ret < 0)
		dev_warn(dev, "Failed to load module '%s %s'\n",
			 inst->fpga_drv, inst->fpga_drv_args);

	if (inst->debug)
		dev_dbg(dev, "rescan 2\n");

	if (pdev) {
		bus_rescan(inst, pdev->bus, inst->fpga_drv);
		/*bus_rescan(bus, "fpga_enable");*/
	} else
		bus_rescan_full();

	if (inst->debug)
		dev_dbg(dev, "CvP device reattach done\n");

	sysfs_notify(&inst->kobj_fpga_dir, NULL, "status");

	return size;

err:
	return ret ? ret : size;
}

#define FPGA_CFG_ATTR_RO(_name) \
	struct fpga_cfg_attribute fpga_cfg_attr_##_name = \
	__ATTR(_name, S_IRUGO, show_##_name, NULL)

#define FPGA_CFG_ATTR_RW(_name) \
	struct fpga_cfg_attribute fpga_cfg_attr_##_name = \
	__ATTR(_name, S_IRUGO | S_IWUSR, show_##_name, store_##_name)

/*static FPGA_CFG_ATTR_RO(history);*/
static FPGA_CFG_ATTR_RW(debug);
static FPGA_CFG_ATTR_RW(load);
static FPGA_CFG_ATTR_RO(status);
static FPGA_CFG_ATTR_RO(ready);

static struct attribute *fpga_cfg_sysfs_attrs[] = {
	/*&fpga_cfg_attr_history.attr,*/
	&fpga_cfg_attr_debug.attr,
	&fpga_cfg_attr_load.attr,
	&fpga_cfg_attr_ready.attr,
	&fpga_cfg_attr_status.attr,
	NULL,
	NULL
};

static struct kobj_type fpga_cfg_ktype = {
	.sysfs_ops = &fpga_cfg_sysfs_ops,
	.default_attrs = fpga_cfg_sysfs_attrs,
};

static ssize_t show_image(struct fpga_cfg_fpga_inst *inst, struct attribute *attr, char *buf);
static ssize_t show_meta(struct fpga_cfg_fpga_inst *inst, struct attribute *attr, char *buf);

#define FPGA_CFG_ATTR_FPP_RO(_name) \
	struct fpga_cfg_attribute fpga_cfg_attr_fpp_##_name = \
	__ATTR(_name, S_IRUGO, show_##_name, NULL)

#define FPGA_CFG_ATTR_SPI_RO(_name) \
	struct fpga_cfg_attribute fpga_cfg_attr_spi_##_name = \
	__ATTR(_name, S_IRUGO, show_##_name, NULL)

#define FPGA_CFG_ATTR_CVP_RO(_name) \
	struct fpga_cfg_attribute fpga_cfg_attr_cvp_##_name = \
	__ATTR(_name, S_IRUGO, show_##_name, NULL)

static FPGA_CFG_ATTR_FPP_RO(image);
static FPGA_CFG_ATTR_FPP_RO(meta);
static FPGA_CFG_ATTR_SPI_RO(image);
static FPGA_CFG_ATTR_SPI_RO(meta);
static FPGA_CFG_ATTR_CVP_RO(image);
static FPGA_CFG_ATTR_CVP_RO(meta);

static struct attribute *fpga_cfg_fpp_attrs[] = {
	&fpga_cfg_attr_fpp_image.attr,
	&fpga_cfg_attr_fpp_meta.attr,
	NULL
};

static struct attribute *fpga_cfg_spi_attrs[] = {
	&fpga_cfg_attr_spi_image.attr,
	&fpga_cfg_attr_spi_meta.attr,
	NULL
};

static struct attribute *fpga_cfg_cvp_attrs[] = {
	&fpga_cfg_attr_cvp_image.attr,
	&fpga_cfg_attr_cvp_meta.attr,
	NULL
};

static struct attribute_group fpga_cfg_fpp_attribute_group = {
	.name	= "fpp",
	.attrs	= fpga_cfg_fpp_attrs
};

static struct attribute_group fpga_cfg_spi_attribute_group = {
	.name	= "spi",
	.attrs	= fpga_cfg_spi_attrs
};

static struct attribute_group fpga_cfg_cvp_attribute_group = {
	.name	= "cvp",
	.attrs	= fpga_cfg_cvp_attrs
};

static ssize_t show_image(struct fpga_cfg_fpga_inst *inst,
			  struct attribute *attr, char *buf)
{
	struct cfg_desc *desc;

	if (attr == &inst->pr_image_attr.attr)
		desc = &inst->pr;
	else if (attr == fpga_cfg_fpp_attrs[0])
		desc = &inst->fpp;
	else if (attr == fpga_cfg_spi_attrs[0])
		desc = &inst->spi;
	else if (attr == fpga_cfg_cvp_attrs[0])
		desc = &inst->cvp;
	else
		return -EINVAL;

	return snprintf(buf, PATH_MAX + NAME_MAX, "%s\n", desc->firmware_abs);
}

static ssize_t show_meta(struct fpga_cfg_fpga_inst *inst,
			 struct attribute *attr, char *buf)
{
	struct cfg_desc *desc;

	if (attr == &inst->pr_meta_attr.attr)
		desc = &inst->pr;
	else if (attr == fpga_cfg_fpp_attrs[1])
		desc = &inst->fpp;
	else if (attr == fpga_cfg_spi_attrs[1])
		desc = &inst->spi;
	else if (attr == fpga_cfg_cvp_attrs[1])
		desc = &inst->cvp;
	else
		return -EINVAL;

	return snprintf(buf, PATH_MAX + NAME_MAX, "%s\n", desc->metadata_abs);
}

struct debugfs_entry {
	char *name;
};

struct debugfs_entry entries[] = {
	{ "cvp" },
	{ "pr" },
	{ "debug" },
	{ "load" },
	{ "ready" },
	{ "status" },
	{ NULL },
};

static char target_buf[PATH_MAX];

static void create_debugfs_entry(struct dentry *dir, int dev_idx, char *name)
{
	sprintf(target_buf, "/sys/devices/platform/fpga-cfg.%d/fpga%d/%s",
		dev_idx, dev_idx, name);
	debugfs_create_symlink(name, dir, target_buf);
}

static void create_debugfs_entries(struct fpga_cfg *priv, int dev_idx)
{
	int i = 0;

	while (entries[i].name) {
		create_debugfs_entry(priv->dbgfs_devdir, dev_idx,
				     entries[i++].name);
	}
}

static int fpga_cfg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpga_cfg_platform_data *pdata;
	struct fpga_cfg_fpga_inst *inst;
	struct fpga_manager *mgr = NULL;
	struct fpga_cfg *priv;
	enum fpga_cfg_mgr_type mgr_type;
	char fname[32];
	int ret;

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata && !pdata->mgr) {
		dev_err(dev, "Missing fpga-cfg pdata...\n");
		return -ENODEV;
	}

	dev_dbg(dev, "probing, fpga mgr in pdata %s\n", pdata->mgr->name);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(dev, "Can't allocate memory for priv data\n");
		return -ENOMEM;
	}

	inst = &priv->fpga;

	/* Create sub-directory for fpga config interface */
	sprintf(fname, "fpga%d", pdev->id);
	priv->dbgfs_devdir = debugfs_create_dir(fname, dbgfs_root);
	if (!priv->dbgfs_devdir) {
		dev_err(&pdev->dev, "Can't create debugfs dir '%s'\n", fname);
		return -ENOENT;
	}

	inst->dbgfs_history = debugfs_create_file("history", 0444,
						priv->dbgfs_devdir, inst,
						&dbgfs_history_ops);
	if (!inst->dbgfs_history) {
		dev_err(&pdev->dev, "Can't create debugfs history entry\n");
		debugfs_remove(priv->dbgfs_devdir);
		return -ENOENT;
	}

	priv->pdev = pdev;
	platform_set_drvdata(pdev, priv);

	mgr_type = pdata->mgr_type;

	if (mgr_type == SPI_RING_MGR || mgr_type == SPI_MGR ||
	    mgr_type == FPP_RING_MGR) {
		mgr = fpga_mgr_get(pdata->mgr->dev.parent);
		if (IS_ERR(mgr)) {
			dev_err(dev, "Failed getting fpga mgr '%s': %ld\n",
				pdata->mgr->name, PTR_ERR(mgr));
			debugfs_remove_recursive(priv->dbgfs_devdir);
			return PTR_ERR(mgr);
		}
	}

	priv->fpga.mgr_type = pdata->mgr_type;
	priv->fpga.cfg = priv;

	if (mgr_type == FPP_RING_MGR) {
		priv->fpga.fpp.mgr = mgr;
		priv->fpga.fpp.mgr_dev = mgr->dev.parent;
		ret = sscanf(mgr->name, "%s %s", fpga_cfg_mgr_name_buf,
			     priv->fpga.usb_dev_id);
		if (ret != 2) {
			dev_err(dev,
				"Can't find usb id in mgr name: %d\n", ret);
			ret = -EINVAL;
			goto err0;
		}
		dev_dbg(dev, "FPP manager usb id: '%s'\n",
			priv->fpga.usb_dev_id);
	}

	if (mgr_type == SPI_RING_MGR || mgr_type == SPI_MGR) {
		priv->fpga.spi.mgr = mgr;
		priv->fpga.spi.mgr_dev = mgr->dev.parent;
	}

	priv->fpga.history_max_entries = fpgacfg_hist_len;

	mutex_init(&priv->fpga.history_lock);
	INIT_LIST_HEAD(&priv->fpga.history_list);
	init_waitqueue_head(&priv->fpga.wait_queue);
	init_waitqueue_head(&priv->fpga.hist_queue);

	ret = kobject_init_and_add(&priv->fpga.kobj_fpga_dir,
				   &fpga_cfg_ktype, &pdev->dev.kobj,
				   "fpga%d", pdev->id);
	if (ret) {
		kobject_put(&priv->fpga.kobj_fpga_dir);
		dev_err(&pdev->dev, "Can't create sysfs entry\n");
		goto err0;
	}

	if (priv->fpga.fpp.mgr) {
		ret = sysfs_create_group(&priv->fpga.kobj_fpga_dir,
					 &fpga_cfg_fpp_attribute_group);
		if (ret) {
			dev_err(&pdev->dev, "Can't add fpp group\n");
			goto err1;
		}
	}
	if (priv->fpga.spi.mgr) {
		ret = sysfs_create_group(&priv->fpga.kobj_fpga_dir,
					 &fpga_cfg_spi_attribute_group);
		if (ret) {
			dev_err(&pdev->dev, "Can't add spi group\n");
			goto err1;
		}
	}

	if (priv->fpga.fpp.mgr ||
	    (priv->fpga.spi.mgr && priv->fpga.mgr_type == SPI_RING_MGR)) {
		ret = sysfs_create_group(&priv->fpga.kobj_fpga_dir,
					 &fpga_cfg_cvp_attribute_group);
		if (ret) {
			dev_err(&pdev->dev, "Can't add cvp group\n");
			goto err2;
		}
	}

	if (priv->fpga.fpp.mgr) {
		snprintf(priv->fpga.pr_image_attr_name,
			 sizeof(priv->fpga.pr_image_attr_name),
			 "image%zi", priv->fpga.pr_seq_num);
		snprintf(priv->fpga.pr_meta_attr_name,
			 sizeof(priv->fpga.pr_meta_attr_name),
			 "meta%zi", priv->fpga.pr_seq_num);

		priv->fpga.pr_image_attr.attr.name = inst->pr_image_attr_name;
		priv->fpga.pr_image_attr.attr.mode = S_IRUGO;
		priv->fpga.pr_image_attr.show = show_image;
		sysfs_attr_init(&priv->fpga.pr_image_attr.attr);
		priv->fpga.pr_meta_attr.attr.name = inst->pr_meta_attr_name;
		priv->fpga.pr_meta_attr.attr.mode = S_IRUGO;
		priv->fpga.pr_meta_attr.show = show_meta;
		sysfs_attr_init(&priv->fpga.pr_meta_attr.attr);

		priv->fpga.pr_attrs[0] = &priv->fpga.pr_image_attr.attr;
		priv->fpga.pr_attrs[1] = &priv->fpga.pr_meta_attr.attr;
		priv->fpga.pr_attrs[2] = NULL;
		priv->fpga.pr_attr_grp.name = "pr";
		priv->fpga.pr_attr_grp.attrs = priv->fpga.pr_attrs;

		ret = sysfs_create_group(&priv->fpga.kobj_fpga_dir,
					 &priv->fpga.pr_attr_grp);
		if (ret) {
			dev_err(&pdev->dev, "Cannot add pr group\n");
			goto err3;
		}
	}

	if (priv->fpga.fpp.mgr) {
		create_debugfs_entry(priv->dbgfs_devdir, pdev->id, "fpp");
		create_debugfs_entries(priv, pdev->id);
	}

	if (priv->fpga.spi.mgr) {
		create_debugfs_entry(priv->dbgfs_devdir, pdev->id, "spi");
		if (priv->fpga.mgr_type == SPI_MGR ||
		    priv->fpga.mgr_type == SPI_RING_MGR) {
			create_debugfs_entry(priv->dbgfs_devdir, pdev->id,
					     entries[2].name);
			create_debugfs_entry(priv->dbgfs_devdir, pdev->id,
					     entries[3].name);
			create_debugfs_entry(priv->dbgfs_devdir, pdev->id,
					     entries[5].name);
		}
		if (priv->fpga.mgr_type == SPI_RING_MGR) {
			create_debugfs_entry(priv->dbgfs_devdir, pdev->id,
					     entries[0].name);
		}
	}

	if (mgr)
		dev_dbg(&pdev->dev, "Using FPGA manager '%s'\n", mgr->name);

	return 0;
/*
err4:
	if (priv->fpga.fpp.mgr)
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &inst->pr_attr_grp);
*/
err3:
	if (priv->fpga.fpp.mgr ||
	    (priv->fpga.spi.mgr && priv->fpga.mgr_type == SPI_RING_MGR)) {
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_cvp_attribute_group);
	}
err2:
	if (priv->fpga.fpp.mgr)
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_fpp_attribute_group);
	if (priv->fpga.spi.mgr)
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_spi_attribute_group);
err1:
	kobject_put(&priv->fpga.kobj_fpga_dir);
err0:
	debugfs_remove_recursive(priv->dbgfs_devdir);
	if (mgr && (mgr_type == SPI_RING_MGR || mgr_type == SPI_MGR ||
	    mgr_type == FPP_RING_MGR))
		fpga_mgr_put(mgr);
	return ret;
}

static int fpga_cfg_remove(struct platform_device *pdev)
{
	struct fpga_cfg *priv = platform_get_drvdata(pdev);
	struct fpga_cfg_fpga_inst *inst;

	dev_dbg(&pdev->dev, "removing\n");

	ida_simple_remove(&fpga_cfg_ida, pdev->id);

	inst = &priv->fpga;

	dev_dbg(&pdev->dev, "%s: ID %d: fpp %p, spi %p, cvp %p, pr %p\n",
		 __func__, pdev->id, inst->fpp.mgr, inst->spi.mgr,
		 inst->cvp.mgr, inst->pr.mgr);

	fpga_cfg_free_log(inst);

	if (inst->fpp.mgr) {
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &inst->pr_attr_grp);
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_fpp_attribute_group);
		fpga_mgr_put(inst->fpp.mgr);
	} else if (inst->spi.mgr) {
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_spi_attribute_group);
		fpga_mgr_put(inst->spi.mgr);
	} else if (inst->cvp.mgr)
		fpga_mgr_put(inst->cvp.mgr);
	else if (inst->pr.mgr)
		fpga_mgr_put(inst->pr.mgr);

	if (priv->fpga.fpp.mgr ||
	    (priv->fpga.spi.mgr && priv->fpga.mgr_type == SPI_RING_MGR)) {
		sysfs_remove_group(&inst->kobj_fpga_dir,
				   &fpga_cfg_cvp_attribute_group);
	}

	kobject_put(&inst->kobj_fpga_dir);
	debugfs_remove_recursive(priv->dbgfs_devdir);
	return 0;
}

#ifdef CONFIG_PM
static int fpga_cfg_suspend(struct device *dev)
{
	/*fpga_cfg_cleanup_mgrs();*/
	return 0;
}

static int fpga_cfg_resume(struct device *dev)
{
	return 0;
}
static const struct dev_pm_ops fpga_cfg_pm_ops = {
	.suspend	= fpga_cfg_suspend,
	.resume		= fpga_cfg_resume,
};
#endif

static struct platform_driver fpga_cfg_driver = {
	.driver = {
		.name   = "fpga-cfg",
#ifdef CONFIG_PM
		.pm = &fpga_cfg_pm_ops,
#endif
	},
	.probe = fpga_cfg_probe,
	.remove = fpga_cfg_remove,
};

static int fpga_cfg_dev_probe(struct platform_device *pdev)
{
	int ret;

	if (fpgacfg_hist_len < FPGA_CFG_HISTORY_ENTRIES_MIN) {
		fpgacfg_hist_len = FPGA_CFG_HISTORY_ENTRIES_MIN;
		pr_warn("fpga-cfg: Using min. fpgacfg_hist_len %d\n",
			fpgacfg_hist_len);
	}

	if (fpgacfg_hist_len > FPGA_CFG_HISTORY_ENTRIES_MAX) {
		fpgacfg_hist_len = FPGA_CFG_HISTORY_ENTRIES_MAX;
		pr_warn("fpga-cfg: Using max. fpgacfg_hist_len %d\n",
			fpgacfg_hist_len);
	}

	/* Create debugfs root directory for all FPGA devices */
	dbgfs_root = debugfs_create_dir(FPGA_DRV_STRING, NULL);
	if (IS_ERR_OR_NULL(dbgfs_root)) {
		pr_err("fpga-cfg: failed to create driver's debugfs dir\n");
		return -ENOENT;
	}

	ret = platform_driver_register(&fpga_cfg_driver);
	if (ret)
		goto err;

	fpga_mgr_register_mgr_notifier(&fpga_mgr_notifier);
	return 0;
err:
	pr_err("%s: err: %d\n", __func__, ret);
	debugfs_remove_recursive(dbgfs_root);
	return ret;
}

static int fpga_cfg_dev_remove(struct platform_device *pdev)
{
	fpga_mgr_unregister_mgr_notifier(&fpga_mgr_notifier);
	fpga_cfg_detach_mgrs(fpga_mgr_class);

	fpga_cfg_cleanup_mgrs();
	platform_driver_unregister(&fpga_cfg_driver);
	fpga_mgr_class = NULL;

	ida_destroy(&fpga_cfg_ida);

	if (dbgfs_root) {
		debugfs_remove_recursive(dbgfs_root);
		dbgfs_root = NULL;
	}
	return 0;
}

static struct platform_driver fpga_cfg_dev_driver = {
	.driver = {
		.name   = "fpga-cfg-dev",
	},
	.probe = fpga_cfg_dev_probe,
	.remove = fpga_cfg_dev_remove,
};

module_platform_driver(fpga_cfg_dev_driver);

MODULE_ALIAS("platform:fpga-cfg-dev");
MODULE_AUTHOR("Anatolij Gustschin <agust@denx.de>");
MODULE_DESCRIPTION("FPGA configuration interface driver");
MODULE_LICENSE("GPL v2");
