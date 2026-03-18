#include <linux/version.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/namei.h>
#include <linux/list.h>
#include <linux/init_task.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/fdtable.h>
#include <linux/statfs.h>
#include <linux/random.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/fsnotify_backend.h>
#include <linux/susfs.h>
#include "fuse/fuse_i.h"
#include "mount.h"

extern bool susfs_is_current_ksu_domain(void);

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
bool susfs_is_log_enabled __read_mostly = true;
#define SUSFS_LOGI(fmt, ...) if (READ_ONCE(susfs_is_log_enabled)) pr_info("susfs:[%u][%d][%s] " fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#define SUSFS_LOGE(fmt, ...) if (READ_ONCE(susfs_is_log_enabled)) pr_err("susfs:[%u][%d][%s]" fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#else
#define SUSFS_LOGI(fmt, ...)
#define SUSFS_LOGE(fmt, ...)
#endif

bool susfs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++)
            return false;
    }
    return true;
}

/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
static DEFINE_SPINLOCK(susfs_spin_lock_sus_path);
static LIST_HEAD(LH_SUS_PATH_LOOP);
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif
const struct qstr susfs_fake_qstr_name = QSTR_INIT("..5.u.S", 7);

void susfs_set_i_state_on_external_dir(void __user **user_info) {
    static struct st_external_dir info = {0};

    if (copy_from_user(&info, (struct st_external_dir __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    info.err = 0;
out_copy_to_user:
    if (copy_to_user(&((struct st_external_dir __user*)*user_info)->err, &info.err, sizeof(info.err))) {
        info.err = -EFAULT;
    }
    if (info.cmd == CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH) {
        SUSFS_LOGI("CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH deprecated, ret: %d\n", info.err);
    } else if (info.cmd == CMD_SUSFS_SET_SDCARD_ROOT_PATH) {
        SUSFS_LOGI("CMD_SUSFS_SET_SDCARD_ROOT_PATH, deprecated, ret: %d\n", info.err);
    }
}

void susfs_add_sus_path(void __user **user_info) {
    struct st_susfs_sus_path info = {0};
    struct path path;
    struct inode *inode = NULL;
    struct fuse_inode *fi = NULL;

    if (copy_from_user(&info, (struct st_susfs_sus_path __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }

    info.err = kern_path(info.target_pathname, LOOKUP_FOLLOW, &path);
    if (info.err) {
        SUSFS_LOGE("failed opening file '%s'\n", info.target_pathname);
        goto out_copy_to_user;
    }

    inode = d_backing_inode(path.dentry);
    if (!inode || !inode->i_mapping) {
        SUSFS_LOGE("inode || inode->i_mapping is NULL\n");
        info.err = -ENOENT;
        goto out_path_put_path;
    }

    if (inode->i_sb->s_magic == FUSE_SUPER_MAGIC) {
        fi = get_fuse_inode(inode);
        if (!fi || !fi->inode.i_mapping) {
            SUSFS_LOGE("fi || fi->inode.i_mapping is NULL\n");
            info.err = -ENOENT;
            goto out_path_put_path;
        }
        set_bit(AS_FLAGS_SUS_PATH, &fi->inode.i_mapping->flags);
        info.err = 0;
        goto out_path_put_path;
    }

    set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
    info.err = 0;
out_path_put_path:
    path_put(&path);
out_copy_to_user:
    if (copy_to_user(&((struct st_susfs_sus_path __user*)*user_info)->err, &info.err, sizeof(info.err))) {
        info.err = -EFAULT;
    }
}

void susfs_add_sus_path_loop(void __user **user_info) {
    struct st_susfs_sus_path_list *new_list = NULL;
    struct st_susfs_sus_path info = {0};

    if (copy_from_user(&info, (struct st_susfs_sus_path __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }

    new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
    if (!new_list) {
        info.err = -ENOMEM;
        goto out_copy_to_user;
    }
    new_list->info.target_ino = info.target_ino;
    strncpy(new_list->info.target_pathname, info.target_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
    strncpy(new_list->target_pathname, info.target_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
    new_list->info.i_uid = info.i_uid;
    new_list->path_len = strlen(new_list->info.target_pathname);
    INIT_LIST_HEAD(&new_list->list);
    spin_lock(&susfs_spin_lock_sus_path);
    list_add_tail(&new_list->list, &LH_SUS_PATH_LOOP);
    spin_unlock(&susfs_spin_lock_sus_path);
    info.err = 0;
out_copy_to_user:
    if (copy_to_user(&((struct st_susfs_sus_path __user*)*user_info)->err, &info.err, sizeof(info.err))) {
        info.err = -EFAULT;
    }
}

void susfs_run_sus_path_loop(void) {
    struct st_susfs_sus_path_list *cursor = NULL;
    struct path path;
    struct inode *inode;
    struct fuse_inode *fi = NULL;

    list_for_each_entry(cursor, &LH_SUS_PATH_LOOP, list) {
        if (*cursor->target_pathname != '\0' &&
            !kern_path(cursor->target_pathname, 0, &path))
        {
            inode = d_backing_inode(path.dentry);
            if (!inode || !inode->i_mapping) {
                path_put(&path);
                continue;
            }
            if (inode->i_sb->s_magic == FUSE_SUPER_MAGIC) {
                fi = get_fuse_inode(inode);
                if (fi && fi->inode.i_mapping)
                    set_bit(AS_FLAGS_SUS_PATH, &fi->inode.i_mapping->flags);
            } else {
                set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
            }
            path_put(&path);
        }
    }
}

static inline bool is_i_uid_not_allowed(uid_t i_uid) {
    return likely(current_uid().val != i_uid);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
bool susfs_is_inode_sus_path(struct mnt_idmap* idmap, struct inode *inode)
#else
bool susfs_is_inode_sus_path(struct inode *inode)
#endif
{
    struct fuse_inode *fi = NULL;
    if (!susfs_is_current_proc_umounted_app()) return false;
    if (!inode->i_mapping) return false;

    if (inode->i_sb->s_magic == FUSE_SUPER_MAGIC) {
        fi = get_fuse_inode(inode);
        if (!fi || !fi->inode.i_mapping) return false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
        if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &fi->inode.i_mapping->flags) &&
            is_i_uid_not_allowed(i_uid_into_vfsuid(idmap, &fi->inode).val)))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
        if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &fi->inode.i_mapping->flags) &&
            is_i_uid_not_allowed(i_uid_into_mnt(i_user_ns(&fi->inode), &fi->inode).val)))
#else
        if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &fi->inode.i_mapping->flags) &&
            is_i_uid_not_allowed(fi->inode.i_uid.val)))
#endif
        {
            return true;
        }
        return false;
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags) &&
        is_i_uid_not_allowed(i_uid_into_vfsuid(idmap, inode).val)))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags) &&
        is_i_uid_not_allowed(i_uid_into_mnt(i_user_ns(inode), inode).val)))
#else
    if (unlikely(test_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags) &&
        is_i_uid_not_allowed(inode->i_uid.val)))
#endif
    {
        return true;
    }
    return false;
}
#endif

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
bool susfs_hide_sus_mnts_for_non_su_procs = false;

void susfs_set_hide_sus_mnts_for_non_su_procs(void __user **user_info) {
    struct st_susfs_hide_sus_mnts_for_non_su_procs info = {0};
    if (copy_from_user(&info, (struct st_susfs_hide_sus_mnts_for_non_su_procs __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    WRITE_ONCE(susfs_hide_sus_mnts_for_non_su_procs, info.enabled);
    info.err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_hide_sus_mnts_for_non_su_procs __user*)*user_info)->err, &info.err, sizeof(info.err));
}
#endif

/* sus_kstat */
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
static DEFINE_SPINLOCK(susfs_spin_lock_sus_kstat);
static DEFINE_HASHTABLE(SUS_KSTAT_HLIST, 10);

static int susfs_update_sus_kstat_inode(char *target_pathname) {
    struct path path;
    struct inode *inode = NULL;
    int err = kern_path(target_pathname, 0, &path);
    if (err) return err;
    inode = d_backing_inode(path.dentry);
    if (inode && inode->i_mapping) set_bit(AS_FLAGS_SUS_KSTAT, &inode->i_mapping->flags);
    path_put(&path);
    return 0;
}

void susfs_add_sus_kstat(void __user **user_info) {
    struct st_susfs_sus_kstat info = {0};
    struct st_susfs_sus_kstat_hlist *new_entry;

    if (copy_from_user(&info, (struct st_susfs_sus_kstat __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }

    new_entry = kmalloc(sizeof(struct st_susfs_sus_kstat_hlist), GFP_KERNEL);
    if (!new_entry) {
        info.err = -ENOMEM;
        goto out_copy_to_user;
    }

#if defined(__ARCH_WANT_STAT64) || defined(__ARCH_WANT_COMPAT_STAT64)
#ifdef CONFIG_MIPS
    info.spoofed_dev = new_decode_dev(info.spoofed_dev);
#else
    info.spoofed_dev = huge_decode_dev(info.spoofed_dev);
#endif
#else
    info.spoofed_dev = old_decode_dev(info.spoofed_dev);
#endif

    new_entry->target_ino = info.target_ino;
    memcpy(&new_entry->info, &info, sizeof(info));
    info.err = susfs_update_sus_kstat_inode(new_entry->info.target_pathname);
    if (info.err) {
        kfree(new_entry);
        goto out_copy_to_user;
    }

    spin_lock(&susfs_spin_lock_sus_kstat);
    hash_add(SUS_KSTAT_HLIST, &new_entry->node, info.target_ino);
    spin_unlock(&susfs_spin_lock_sus_kstat);
    info.err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_sus_kstat __user*)*user_info)->err, &info.err, sizeof(info.err));
}

void susfs_update_sus_kstat(void __user **user_info) {
    struct st_susfs_sus_kstat info = {0};
    struct st_susfs_sus_kstat_hlist *new_entry, *tmp_entry;
    struct hlist_node *tmp_node;
    int bkt;

    if (copy_from_user(&info, (struct st_susfs_sus_kstat __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
        
    hash_for_each_safe(SUS_KSTAT_HLIST, bkt, tmp_node, tmp_entry, node) {
        if (!strcmp(tmp_entry->info.target_pathname, info.target_pathname)) {
            info.err = susfs_update_sus_kstat_inode(tmp_entry->info.target_pathname);
            if (info.err) goto out_copy_to_user;
            new_entry = kmalloc(sizeof(struct st_susfs_sus_kstat_hlist), GFP_KERNEL);
            if (!new_entry) { info.err = -ENOMEM; goto out_copy_to_user; }
            memcpy(&new_entry->info, &tmp_entry->info, sizeof(tmp_entry->info));
            new_entry->target_ino = info.target_ino;
            new_entry->info.target_ino = info.target_ino;
            if (info.spoofed_size > 0) new_entry->info.spoofed_size = info.spoofed_size;
            if (info.spoofed_blocks > 0) new_entry->info.spoofed_blocks = info.spoofed_blocks;
            hash_del(&tmp_entry->node);
            kfree(tmp_entry);
            spin_lock(&susfs_spin_lock_sus_kstat);
            hash_add(SUS_KSTAT_HLIST, &new_entry->node, info.target_ino);
            spin_unlock(&susfs_spin_lock_sus_kstat);
            info.err = 0;
            goto out_copy_to_user;
        }
    }
out_copy_to_user:
    copy_to_user(&((struct st_susfs_sus_kstat __user*)*user_info)->err, &info.err, sizeof(info.err));
}

void susfs_sus_ino_for_generic_fillattr(unsigned long ino, struct kstat *stat) {
    struct st_susfs_sus_kstat_hlist *entry;
    hash_for_each_possible(SUS_KSTAT_HLIST, entry, node, ino) {
        if (entry->target_ino == ino) {
            stat->dev = entry->info.spoofed_dev;
            stat->ino = entry->info.spoofed_ino;
            stat->nlink = entry->info.spoofed_nlink;
            stat->size = entry->info.spoofed_size;
            stat->atime.tv_sec = entry->info.spoofed_atime_tv_sec;
            stat->atime.tv_nsec = entry->info.spoofed_atime_tv_nsec;
            stat->mtime.tv_sec = entry->info.spoofed_mtime_tv_sec;
            stat->mtime.tv_nsec = entry->info.spoofed_mtime_tv_nsec;
            stat->ctime.tv_sec = entry->info.spoofed_ctime_tv_sec;
            stat->ctime.tv_nsec = entry->info.spoofed_ctime_tv_nsec;
            stat->blocks = entry->info.spoofed_blocks;
            stat->blksize = entry->info.spoofed_blksize;
            return;
        }
    }
}

void susfs_sus_ino_for_show_map_vma(unsigned long ino, dev_t *out_dev, unsigned long *out_ino) {
    struct st_susfs_sus_kstat_hlist *entry;
    hash_for_each_possible(SUS_KSTAT_HLIST, entry, node, ino) {
        if (entry->target_ino == ino) {
            *out_dev = entry->info.spoofed_dev;
            *out_ino = entry->info.spoofed_ino;
            return;
        }
    }
}
#endif

/* spoof_uname */
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
static struct st_susfs_uname my_uname = {0};
static bool is_susfs_uname_set = false;
static DEFINE_SEQLOCK(susfs_uname_seqlock);

void susfs_set_uname(void __user **user_info) {
    struct st_susfs_uname info = {0};
    if (copy_from_user(&info, (struct st_susfs_uname __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    write_seqlock(&susfs_uname_seqlock);
    if (!strcmp(info.release, "default")) strscpy(my_uname.release, utsname()->release, __NEW_UTS_LEN);
    else strncpy(my_uname.release, info.release, __NEW_UTS_LEN);
    if (!strcmp(info.version, "default")) strscpy(my_uname.version, utsname()->version, __NEW_UTS_LEN);
    else strncpy(my_uname.version, info.version, __NEW_UTS_LEN);
    is_susfs_uname_set = true;
    write_sequnlock(&susfs_uname_seqlock);
    info.err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_uname __user*)*user_info)->err, &info.err, sizeof(info.err));
}

void susfs_spoof_uname(struct new_utsname* tmp) {
    unsigned seq;
    do {
        seq = read_seqbegin(&susfs_uname_seqlock);
        if (is_susfs_uname_set) {
            strncpy(tmp->release, my_uname.release, __NEW_UTS_LEN);
            strncpy(tmp->version, my_uname.version, __NEW_UTS_LEN);
        }
    } while (read_seqretry(&susfs_uname_seqlock, seq));
}
#endif

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_enable_log(void __user **user_info) {
    struct st_susfs_log info = {0};
    if (copy_from_user(&info, (struct st_susfs_log __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    WRITE_ONCE(susfs_is_log_enabled, info.enabled);
    info.err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_log __user*)*user_info)->err, &info.err, sizeof(info.err));
}
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
static char *fake_cmdline_or_bootconfig = NULL;
static bool susfs_is_fake_cmdline_or_bootconfig_set = false;
static DEFINE_SEQLOCK(susfs_fake_cmdline_or_bootconfig_seqlock);

void susfs_set_cmdline_or_bootconfig(void __user **user_info) {
    struct st_susfs_spoof_cmdline_or_bootconfig *info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info) return;
    if (copy_from_user(info, (struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info, sizeof(*info))) {
        info->err = -EFAULT;
        goto out_copy_to_user;
    }
    if (!fake_cmdline_or_bootconfig) fake_cmdline_or_bootconfig = kzalloc(SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE, GFP_KERNEL);
    if (!fake_cmdline_or_bootconfig) { info->err = -ENOMEM; goto out_copy_to_user; }
    write_seqlock(&susfs_fake_cmdline_or_bootconfig_seqlock);
    strncpy(fake_cmdline_or_bootconfig, info->fake_cmdline_or_bootconfig, SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE-1);
    susfs_is_fake_cmdline_or_bootconfig_set = true;
    write_sequnlock(&susfs_fake_cmdline_or_bootconfig_seqlock);
    info->err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info)->err, &info->err, sizeof(info->err));
    kfree(info);
}

int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m) {
    unsigned seq;
    int err = -EINVAL;
    do {
        seq = read_seqbegin(&susfs_fake_cmdline_or_bootconfig_seqlock);
        if (susfs_is_fake_cmdline_or_bootconfig_set) {
            seq_puts(m, fake_cmdline_or_bootconfig);
            err = 0;
        }
    } while (read_seqretry(&susfs_fake_cmdline_or_bootconfig_seqlock, seq));
    return err;
}
#endif

/* open_redirect */
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
static DEFINE_SPINLOCK(susfs_spin_lock_open_redirect);
static DEFINE_HASHTABLE(OPEN_REDIRECT_HLIST, 10);

static int susfs_update_open_redirect_inode(struct st_susfs_open_redirect_hlist *new_entry) {
    struct path path;
    int err = kern_path(new_entry->target_pathname, LOOKUP_FOLLOW, &path);
    if (err) return err;
    struct inode *inode = d_backing_inode(path.dentry);
    if (inode && inode->i_mapping) set_bit(AS_FLAGS_OPEN_REDIRECT, &inode->i_mapping->flags);
    path_put(&path);
    return 0;
}

void susfs_add_open_redirect(void __user **user_info) {
    struct st_susfs_open_redirect info = {0};
    struct st_susfs_open_redirect_hlist *new_entry;
    if (copy_from_user(&info, (struct st_susfs_open_redirect __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
    if (!new_entry) { info.err = -ENOMEM; goto out_copy_to_user; }
    new_entry->target_ino = info.target_ino;
    strncpy(new_entry->target_pathname, info.target_pathname, SUSFS_MAX_LEN_PATHNAME-1);
    strncpy(new_entry->redirected_pathname, info.redirected_pathname, SUSFS_MAX_LEN_PATHNAME-1);
    if (susfs_update_open_redirect_inode(new_entry)) { kfree(new_entry); info.err = -EINVAL; goto out_copy_to_user; }
    spin_lock(&susfs_spin_lock_open_redirect);
    hash_add(OPEN_REDIRECT_HLIST, &new_entry->node, info.target_ino);
    spin_unlock(&susfs_spin_lock_open_redirect);
    info.err = 0;
out_copy_to_user:
    copy_to_user(&((struct st_susfs_open_redirect __user*)*user_info)->err, &info.err, sizeof(info.err));
}

struct filename* susfs_get_redirected_path(unsigned long ino) {
    struct st_susfs_open_redirect_hlist *entry;
    hash_for_each_possible(OPEN_REDIRECT_HLIST, entry, node, ino) {
        if (entry->target_ino == ino) return getname_kernel(entry->redirected_pathname);
    }
    return ERR_PTR(-ENOENT);
}
#endif

/* sus_map */
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
void susfs_add_sus_map(void __user **user_info) {
    struct st_susfs_sus_map info = {0};
    struct path path;
    if (copy_from_user(&info, (struct st_susfs_sus_map __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }
    info.err = kern_path(info.target_pathname, LOOKUP_FOLLOW, &path);
    if (!info.err) {
        struct inode *inode = d_backing_inode(path.dentry);
        if (inode && inode->i_mapping) set_bit(AS_FLAGS_SUS_MAP, &inode->i_mapping->flags);
        path_put(&path);
    }
out_copy_to_user:
    copy_to_user(&((struct st_susfs_sus_map __user*)*user_info)->err, &info.err, sizeof(info.err));
}
#endif

/* susfs avc log spoofing */
// FIXED: Definition provided to resolve "undefined reference"
bool susfs_is_avc_log_spoofing_enabled = false;

void susfs_set_avc_log_spoofing(void __user **user_info) {
    struct st_susfs_avc_log_spoofing info = {0};

    if (copy_from_user(&info, (struct st_susfs_avc_log_spoofing __user*)*user_info, sizeof(info))) {
        info.err = -EFAULT;
        goto out_copy_to_user;
    }

    WRITE_ONCE(susfs_is_avc_log_spoofing_enabled, info.enabled);
    SUSFS_LOGI("susfs_is_avc_log_spoofing_enabled: %d\n", info.enabled);
    info.err = 0;
out_copy_to_user:
    if (copy_to_user(&((struct st_susfs_avc_log_spoofing __user*)*user_info)->err, &info.err, sizeof(info.err))) {
        info.err = -EFAULT;
    }
    SUSFS_LOGI("CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING -> ret: %d\n", info.err);
}

/* get susfs enabled features */
static int copy_config_to_buf(const char *config_string, char *buf_ptr, size_t *copied_size, size_t bufsize) {
    size_t tmp_size = strlen(config_string);
    *copied_size += tmp_size;
    if (*copied_size >= bufsize) return -EINVAL;
    strncpy(buf_ptr, config_string, tmp_size);
    return 0;
}

void susfs_get_enabled_features(void __user **user_info) {
    struct st_susfs_enabled_features *info = kzalloc(sizeof(*info), GFP_KERNEL);
    char *buf_ptr;
    size_t copied_size = 0;

    if (!info) return;
    if (copy_from_user(info, (struct st_susfs_enabled_features __user*)*user_info, sizeof(*info))) {
        info->err = -EFAULT;
        goto out_copy_to_user;
    }

    buf_ptr = info->enabled_features;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
    if (!copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_PATH\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE))
        buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
    if (!copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_MOUNT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE))
        buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
    if (!copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_KSTAT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE))
        buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
    if (!copy_config_to_buf("CONFIG_KSU_SUSFS_SPOOF_UNAME\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE))
        buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
    if (!copy_config_to_buf("CONFIG_KSU_SUSFS_ENABLE_LOG\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE))
        buf_ptr = info->enabled_features + copied_size;
#endif

out_copy_to_user:
    copy_to_user(&((struct st_susfs_enabled_features __user*)*user_info)->err, &info->err, sizeof(info->err));
    kfree(info);
}
