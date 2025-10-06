#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/stat.h>
#include <linux/namei.h>

#include "allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "manager.h"
#include "throne_tracker.h"
#include "kernel_compat.h"
#include "dynamic_manager.h"
#include "throne_comm.h"

uid_t ksu_manager_uid = KSU_INVALID_UID;
static uid_t locked_manager_uid = KSU_INVALID_UID;
static uid_t locked_dynamic_manager_uid = KSU_INVALID_UID;

#define KSU_UID_LIST_PATH "/data/misc/user_uid/uid_list"
#define USER_DATA_PATH "/data/user_de/0"
#define USER_DATA_PATH_LEN 288

struct uid_data {
	struct list_head list;
	u32 uid;
	char package[KSU_MAX_PACKAGE_NAME];
};

// Try read /data/misc/user_uid/uid_list
static int uid_from_um_list(struct list_head *uid_list)
{
	struct file *fp;
	char *buf = NULL;
	loff_t size, pos = 0;
	ssize_t nr;
	int cnt = 0;

	fp = ksu_filp_open_compat(KSU_UID_LIST_PATH, O_RDONLY, 0);
	if (IS_ERR(fp))
		return -ENOENT;

	size = fp->f_inode->i_size;
	if (size <= 0) {
		filp_close(fp, NULL);
		return -ENODATA;
	}

	buf = kzalloc(size + 1, GFP_ATOMIC);
	if (!buf) {
		pr_err("uid_list: OOM %lld B\n", size);
		filp_close(fp, NULL);
		return -ENOMEM;
	}

	nr = ksu_kernel_read_compat(fp, buf, size, &pos);
	filp_close(fp, NULL);
	if (nr != size) {
		pr_err("uid_list: short read %zd/%lld\n", nr, size);
		kfree(buf);
		return -EIO;
	}
	buf[size] = '\0';

	for (char *line = buf, *next; line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = '\0';

		while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
		if (!*line) continue;

		char *uid_str = strsep(&line, " \t");
		char *pkg     = line;
		if (!pkg) continue;
		while (*pkg == ' ' || *pkg == '\t') ++pkg;
		if (!*pkg)   continue;

		u32 uid;
		if (kstrtou32(uid_str, 10, &uid)) {
			pr_warn_once("uid_list: bad uid <%s>\n", uid_str);
			continue;
		}

		struct uid_data *d = kzalloc(sizeof(*d), GFP_ATOMIC);
		if (unlikely(!d)) {
			pr_err("uid_list: OOM uid=%u\n", uid);
			continue;
		}

		d->uid = uid;
		strscpy(d->package, pkg, KSU_MAX_PACKAGE_NAME);
		list_add_tail(&d->list, uid_list);
		++cnt;
	}

	kfree(buf);
	pr_info("uid_list: loaded %d entries\n", cnt);
	return cnt > 0 ? 0 : -ENODATA;
}

static int get_pkg_from_apk_path(char *pkg, const char *path)
{
	int len = strlen(path);
	if (len >= KSU_MAX_PACKAGE_NAME || len < 1)
		return -1;

	const char *last_slash = NULL;
	const char *second_last_slash = NULL;

	int i;
	for (i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			if (!last_slash) {
				last_slash = &path[i];
			} else {
				second_last_slash = &path[i];
				break;
			}
		}
	}

	if (!last_slash || !second_last_slash)
		return -1;

	const char *last_hyphen = strchr(second_last_slash, '-');
	if (!last_hyphen || last_hyphen > last_slash)
		return -1;

	int pkg_len = last_hyphen - second_last_slash - 1;
	if (pkg_len >= KSU_MAX_PACKAGE_NAME || pkg_len <= 0)
		return -1;

	// Copying the package name
	strncpy(pkg, second_last_slash + 1, pkg_len);
	pkg[pkg_len] = '\0';

	return 0;
}

static void crown_manager(const char *apk, struct list_head *uid_data, int signature_index)
{
	char pkg[KSU_MAX_PACKAGE_NAME];
	if (get_pkg_from_apk_path(pkg, apk) < 0) {
		pr_err("Failed to get package name from apk path: %s\n", apk);
		return;
	}

	pr_info("manager pkg: %s, signature_index: %d\n", pkg, signature_index);

#ifdef KSU_MANAGER_PACKAGE
	// pkg is `/<real package>`
	if (strncmp(pkg, KSU_MANAGER_PACKAGE, sizeof(KSU_MANAGER_PACKAGE))) {
		pr_info("manager package is inconsistent with kernel build: %s\n",
			KSU_MANAGER_PACKAGE);
		return;
	}
#endif
	struct uid_data *np;

	list_for_each_entry(np, uid_data, list) {
		if (strncmp(np->package, pkg, KSU_MAX_PACKAGE_NAME) == 0) {
			bool is_dynamic = (signature_index == DYNAMIC_SIGN_INDEX || signature_index >= 2);

			if (is_dynamic) {
				if (locked_dynamic_manager_uid != KSU_INVALID_UID && locked_dynamic_manager_uid != np->uid) {
					pr_info("Unlocking previous dynamic manager UID: %d\n", locked_dynamic_manager_uid);
					ksu_remove_manager(locked_dynamic_manager_uid);
					locked_dynamic_manager_uid = KSU_INVALID_UID;
				}
			} else {
				if (locked_manager_uid != KSU_INVALID_UID && locked_manager_uid != np->uid) {
					pr_info("Unlocking previous manager UID: %d\n", locked_manager_uid);
					ksu_invalidate_manager_uid(); // unlock old one
					locked_manager_uid = KSU_INVALID_UID;
				}
			}

			pr_info("Crowning %s manager: %s (uid=%d, signature_index=%d)\n",
			        is_dynamic ? "dynamic" : "traditional", pkg, np->uid, signature_index);

			if (is_dynamic) {
				ksu_add_manager(np->uid, signature_index);
				locked_dynamic_manager_uid = np->uid;

				// If there is no traditional manager, set it to the current UID
				if (!ksu_is_manager_uid_valid()) {
					ksu_set_manager_uid(np->uid);
					locked_manager_uid = np->uid;
				}
			} else {
				ksu_set_manager_uid(np->uid); // throne new UID
				locked_manager_uid = np->uid; // store locked UID
			}
			break;
		}
	}
}

#define DATA_PATH_LEN 384 // 384 is enough for /data/app/<package>/base.apk

struct data_path {
	char dirpath[DATA_PATH_LEN];
	int depth;
	struct list_head list;
};

struct apk_path_hash {
	unsigned int hash;
	bool exists;
	struct list_head list;
};

static struct list_head apk_path_hash_list = LIST_HEAD_INIT(apk_path_hash_list);

struct my_dir_context {
	struct dir_context ctx;
	struct list_head *data_path_list;
	char *parent_dir;
	void *private_data;
	int depth;
	int *stop;
};
// https://docs.kernel.org/filesystems/porting.html
// filldir_t (readdir callbacks) calling conventions have changed. Instead of returning 0 or -E... it returns bool now. false means "no more" (as -E... used to) and true - "keep going" (as 0 in old calling conventions). Rationale: callers never looked at specific -E... values anyway. -> iterate_shared() instances require no changes at all, all filldir_t ones in the tree converted.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define FILLDIR_RETURN_TYPE bool
#define FILLDIR_ACTOR_CONTINUE true
#define FILLDIR_ACTOR_STOP false
#else
#define FILLDIR_RETURN_TYPE int
#define FILLDIR_ACTOR_CONTINUE 0
#define FILLDIR_ACTOR_STOP -EINVAL
#endif

struct uid_scan_stats {
	size_t total_found;
	size_t errors_encountered;
};

struct user_data_context {
	struct dir_context ctx;
	struct list_head *uid_list;
	struct uid_scan_stats *stats;
};

FILLDIR_RETURN_TYPE user_data_actor(struct dir_context *ctx, const char *name,
				     int namelen, loff_t off, u64 ino,
				     unsigned int d_type)
{
	struct user_data_context *my_ctx = 
		container_of(ctx, struct user_data_context, ctx);
	
	if (!my_ctx || !my_ctx->uid_list) {
		return FILLDIR_ACTOR_STOP;
	}

	if (!strncmp(name, "..", namelen) || !strncmp(name, ".", namelen))
		return FILLDIR_ACTOR_CONTINUE;

	if (d_type != DT_DIR)
		return FILLDIR_ACTOR_CONTINUE;

	if (namelen >= KSU_MAX_PACKAGE_NAME) {
		pr_warn("Package name too long: %.*s\n", namelen, name);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	char package_path[USER_DATA_PATH_LEN];
	if (snprintf(package_path, sizeof(package_path), "%s/%.*s", 
		     USER_DATA_PATH, namelen, name) >= sizeof(package_path)) {
		pr_err("Path too long for package: %.*s\n", namelen, name);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	struct path path;
	int err = kern_path(package_path, LOOKUP_FOLLOW, &path);
	if (err) {
		pr_debug("Package path lookup failed: %s (err: %d)\n", package_path, err);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	struct kstat stat;
	err = vfs_getattr(&path, &stat, STATX_UID, AT_STATX_SYNC_AS_STAT);
	path_put(&path);
	
	if (err) {
		pr_info("Failed to get attributes for: %s (err: %d)\n", package_path, err);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	uid_t uid = from_kuid(&init_user_ns, stat.uid);
	if (uid == (uid_t)-1) {
		pr_warn("Invalid UID for package: %.*s\n", namelen, name);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	struct uid_data *data = kzalloc(sizeof(struct uid_data), GFP_ATOMIC);
	if (!data) {
		pr_err("Failed to allocate memory for package: %.*s\n", namelen, name);
		if (my_ctx->stats)
			my_ctx->stats->errors_encountered++;
		return FILLDIR_ACTOR_CONTINUE;
	}

	data->uid = uid;
	size_t copy_len = min(namelen, KSU_MAX_PACKAGE_NAME - 1);
	strncpy(data->package, name, copy_len);
	data->package[copy_len] = '\0';
	
	list_add_tail(&data->list, my_ctx->uid_list);
	
	if (my_ctx->stats)
		my_ctx->stats->total_found++;
	
	pr_info("UserDE UID: Found package: %s, uid: %u\n", data->package, data->uid);
	
	return FILLDIR_ACTOR_CONTINUE;
}

static int scan_user_data_for_uids(struct list_head *uid_list)
{
	struct file *dir_file;
	struct uid_scan_stats stats = {0};
	int ret = 0;
	
	if (!uid_list) {
		return -EINVAL;
	}

	dir_file = ksu_filp_open_compat(USER_DATA_PATH, O_RDONLY, 0);
	if (IS_ERR(dir_file)) {
		pr_err("UserDE UID: Failed to open %s, err: (%ld)\n", USER_DATA_PATH, PTR_ERR(dir_file));
		return PTR_ERR(dir_file);
	}

	struct user_data_context ctx = {
		.ctx.actor = user_data_actor,
		.uid_list = uid_list,
		.stats = &stats
	};

	ret = iterate_dir(dir_file, &ctx.ctx);
	filp_close(dir_file, NULL);

	if (stats.errors_encountered > 0) {
		pr_warn("Encountered %zu errors while scanning user data directory\n", 
			stats.errors_encountered);
	}

	pr_info("UserDE UID: Scanned %s directory with %zu errors\n", 
		USER_DATA_PATH, stats.errors_encountered);

	return ret;
}

FILLDIR_RETURN_TYPE my_actor(struct dir_context *ctx, const char *name,
			     int namelen, loff_t off, u64 ino,
			     unsigned int d_type)
{
	struct my_dir_context *my_ctx =
		container_of(ctx, struct my_dir_context, ctx);
	char dirpath[DATA_PATH_LEN];

	if (!my_ctx) {
		pr_err("Invalid context\n");
		return FILLDIR_ACTOR_STOP;
	}
	if (my_ctx->stop && *my_ctx->stop) {
		pr_info("Stop searching\n");
		return FILLDIR_ACTOR_STOP;
	}

	if (!strncmp(name, "..", namelen) || !strncmp(name, ".", namelen))
		return FILLDIR_ACTOR_CONTINUE; // Skip "." and ".."

	if (d_type == DT_DIR && namelen >= 8 && !strncmp(name, "vmdl", 4) &&
 	    !strncmp(name + namelen - 4, ".tmp", 4)) {
 		pr_info("Skipping directory: %.*s\n", namelen, name);
 		return FILLDIR_ACTOR_CONTINUE; // Skip staging package
 	}
 

	if (snprintf(dirpath, DATA_PATH_LEN, "%s/%.*s", my_ctx->parent_dir,
		     namelen, name) >= DATA_PATH_LEN) {
		pr_err("Path too long: %s/%.*s\n", my_ctx->parent_dir, namelen,
		       name);
		return FILLDIR_ACTOR_CONTINUE;
	}

	if (d_type == DT_DIR && my_ctx->depth > 0 &&
	    (my_ctx->stop && !*my_ctx->stop)) {
		struct data_path *data = kmalloc(sizeof(struct data_path), GFP_ATOMIC);

		if (!data) {
			pr_err("Failed to allocate memory for %s\n", dirpath);
			return FILLDIR_ACTOR_CONTINUE;
		}

		strscpy(data->dirpath, dirpath, DATA_PATH_LEN);
		data->depth = my_ctx->depth - 1;
		list_add_tail(&data->list, my_ctx->data_path_list);
	} else {
		if ((namelen == 8) && (strncmp(name, "base.apk", namelen) == 0)) {
			struct apk_path_hash *pos, *n;
			unsigned int hash = full_name_hash(NULL, dirpath, strlen(dirpath));
			list_for_each_entry(pos, &apk_path_hash_list, list) {
				if (hash == pos->hash) {
					pos->exists = true;
					return FILLDIR_ACTOR_CONTINUE;
				}
			}

			int signature_index = -1;
			bool is_multi_manager = is_dynamic_manager_apk(
				dirpath, &signature_index);

			pr_info("Found new base.apk at path: %s, is_multi_manager: %d, signature_index: %d\n",
				dirpath, is_multi_manager, signature_index);
				
			// Check for dynamic sign or multi-manager signatures
			if (is_multi_manager && (signature_index == DYNAMIC_SIGN_INDEX || signature_index >= 2)) {
				crown_manager(dirpath, my_ctx->private_data, signature_index);
				
				struct apk_path_hash *apk_data = kmalloc(sizeof(struct apk_path_hash), GFP_ATOMIC);
				if (apk_data) {
					apk_data->hash = hash;
					apk_data->exists = true;
					list_add_tail(&apk_data->list, &apk_path_hash_list);
				}

			} else if (is_manager_apk(dirpath)) {
				crown_manager(dirpath, my_ctx->private_data, 0);
				*my_ctx->stop = 1;

				// Manager found, clear APK cache list
				list_for_each_entry_safe(pos, n, &apk_path_hash_list, list) {
					list_del(&pos->list);
					kfree(pos);
				}
			} else {
				struct apk_path_hash *apk_data = kmalloc(sizeof(struct apk_path_hash), GFP_ATOMIC);
				if (apk_data) {
					apk_data->hash = hash;
					apk_data->exists = true;
					list_add_tail(&apk_data->list, &apk_path_hash_list);
				}
			}
		}
	}

	return FILLDIR_ACTOR_CONTINUE;
}

void search_manager(const char *path, int depth, struct list_head *uid_data)
{
	int i, stop = 0;
	struct list_head data_path_list;
	INIT_LIST_HEAD(&data_path_list);
	unsigned long data_app_magic = 0;
	
	// Initialize APK cache list
	struct apk_path_hash *pos, *n;
	list_for_each_entry(pos, &apk_path_hash_list, list) {
		pos->exists = false;
	}

	// First depth
	struct data_path data;
	strscpy(data.dirpath, path, DATA_PATH_LEN);
	data.depth = depth;
	list_add_tail(&data.list, &data_path_list);

	for (i = depth; i >= 0; i--) {
		struct data_path *pos, *n;

		list_for_each_entry_safe(pos, n, &data_path_list, list) {
			struct my_dir_context ctx = { .ctx.actor = my_actor,
						      .data_path_list = &data_path_list,
						      .parent_dir = pos->dirpath,
						      .private_data = uid_data,
						      .depth = pos->depth,
						      .stop = &stop };
			struct file *file;

			if (!stop) {
				file = ksu_filp_open_compat(pos->dirpath, O_RDONLY | O_NOFOLLOW, 0);
				if (IS_ERR(file)) {
					pr_err("Failed to open directory: %s, err: %ld\n", pos->dirpath, PTR_ERR(file));
					goto skip_iterate;
				}
				
				// grab magic on first folder, which is /data/app
				if (!data_app_magic) {
					if (file->f_inode->i_sb->s_magic) {
						data_app_magic = file->f_inode->i_sb->s_magic;
						pr_info("%s: dir: %s got magic! 0x%lx\n", __func__, pos->dirpath, data_app_magic);
					} else {
						filp_close(file, NULL);
						goto skip_iterate;
					}
				}
				
				if (file->f_inode->i_sb->s_magic != data_app_magic) {
					pr_info("%s: skip: %s magic: 0x%lx expected: 0x%lx\n", __func__, pos->dirpath, 
						file->f_inode->i_sb->s_magic, data_app_magic);
					filp_close(file, NULL);
					goto skip_iterate;
				}

				iterate_dir(file, &ctx.ctx);
				filp_close(file, NULL);
			}
skip_iterate:
			list_del(&pos->list);
			if (pos != &data)
				kfree(pos);
		}
	}

	// Remove stale cached APK entries
	list_for_each_entry_safe(pos, n, &apk_path_hash_list, list) {
		if (!pos->exists) {
			list_del(&pos->list);
			kfree(pos);
		}
	}
}

static bool is_uid_exist(uid_t uid, char *package, void *data)
{
	struct list_head *list = (struct list_head *)data;
	struct uid_data *np;

	bool exist = false;
	list_for_each_entry (np, list, list) {
		if (np->uid == uid % 100000 &&
		    strncmp(np->package, package, KSU_MAX_PACKAGE_NAME) == 0) {
			exist = true;
			break;
		}
	}
	return exist;
}

extern bool ksu_uid_scanner_enabled;

void track_throne()
{
	struct list_head uid_list;
	struct uid_data *np, *n;

	// init uid list head
	INIT_LIST_HEAD(&uid_list);

	if (ksu_uid_scanner_enabled) {
		pr_info("Scanning %s directory..\n", KSU_UID_LIST_PATH);
		if (uid_from_um_list(&uid_list) == 0) {
			pr_info("Loaded UIDs from %s success\n", KSU_UID_LIST_PATH);
		} else {
			pr_warn("%s read failed, falling back to %s\n", KSU_UID_LIST_PATH, USER_DATA_PATH);
			if (scan_user_data_for_uids(&uid_list) < 0)
				goto out;
		}
	} else {
		pr_info("User mode scan disabled, scanning %s\n", USER_DATA_PATH);
		if (scan_user_data_for_uids(&uid_list) < 0)
			goto out;
	}

	// check if manager UID exists
	bool manager_exist = false;
	int current_manager_uid = ksu_get_manager_uid() % 100000;

	list_for_each_entry(np, &uid_list, list) {
		if (np->uid == current_manager_uid) {
			manager_exist = true;
			break;
		}
	}

	if (!manager_exist && locked_manager_uid != KSU_INVALID_UID) {
		pr_info("Manager APK removed, unlocking previous UID: %d\n", locked_manager_uid);
		ksu_invalidate_manager_uid();
		locked_manager_uid = KSU_INVALID_UID;
	}

	// Check if the Dynamic Manager exists (only check locked UIDs)
	bool dynamic_manager_exist = false;
	if (ksu_is_dynamic_manager_enabled() && locked_dynamic_manager_uid != KSU_INVALID_UID) {
		list_for_each_entry(np, &uid_list, list) {
			if (np->uid == locked_dynamic_manager_uid) {
				dynamic_manager_exist = true;
				break;
			}
		}

		if (!dynamic_manager_exist) {
			pr_info("Dynamic manager APK removed, unlocking previous UID: %d\n", locked_dynamic_manager_uid);
			ksu_remove_manager(locked_dynamic_manager_uid);
			locked_dynamic_manager_uid = KSU_INVALID_UID;
		}
	}

	bool need_search = !manager_exist;
	if (ksu_is_dynamic_manager_enabled() && !dynamic_manager_exist) {
		need_search = true;
	}

	if (need_search) {
		pr_info("Searching for manager(s)...\n");
		search_manager("/data/app", 2, &uid_list);
		pr_info("Manager search finished\n");
	}

	// then prune the allowlist
	ksu_prune_allowlist(is_uid_exist, &uid_list);
out:
	// free uid_list
	list_for_each_entry_safe(np, n, &uid_list, list) {
		list_del(&np->list);
		kfree(np);
	}
}

void ksu_throne_tracker_init()
{
	// nothing to do
}

void ksu_throne_tracker_exit()
{
	// nothing to do
}
