/*
 * Quota code necessary even when VFS quota support is not compiled
 * into the kernel.  The interesting stuff is over in dquot.c, here
 * we have symbols for initial quotactl(2) handling, the sysctl(2)
 * variables, etc - things needed even when quota support disabled.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <asm/current.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/capability.h>
#include <linux/quotaops.h>
#include <linux/types.h>
#include <linux/writeback.h>
#include <linux/compat.h>

static int check_quotactl_permission(struct super_block *sb, int type, int cmd,
				     qid_t id)
{
	switch (cmd) {
	/* these commands do not require any special privilegues */
	case Q_GETFMT:
	case Q_SYNC:
	case Q_GETINFO:
	case Q_XGETQSTAT:
	case Q_XGETQSTATV:
	case Q_XQUOTASYNC:
		break;
	/* allow to query information for dquots we "own" */
	case Q_GETQUOTA:
	case Q_XGETQUOTA:
		if ((type == USRQUOTA && uid_eq(current_euid(), make_kuid(current_user_ns(), id))) ||
		    (type == GRPQUOTA && in_egroup_p(make_kgid(current_user_ns(), id))))
			break;
		/*FALLTHROUGH*/
	default:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
	}

	return security_quotactl(cmd, type, id, sb);
}

static void quota_sync_one(struct super_block *sb, void *arg)
{
	if (sb->s_qcop && sb->s_qcop->quota_sync)
		sb->s_qcop->quota_sync(sb, *(int *)arg);
}

static int quota_sync_all(int type)
{
	int ret;

	if (type >= MAXQUOTAS)
		return -EINVAL;
	ret = security_quotactl(Q_SYNC, type, 0, NULL);
	if (!ret)
		iterate_supers(quota_sync_one, &type);
	return ret;
}

static int quota_quotaon(struct super_block *sb, int type, int cmd, qid_t id,
		         struct path *path)
{
	if (!sb->s_qcop->quota_on && !sb->s_qcop->quota_on_meta)
		return -ENOSYS;
	if (sb->s_qcop->quota_on_meta)
		return sb->s_qcop->quota_on_meta(sb, type, id);
	if (IS_ERR(path))
		return PTR_ERR(path);
	return sb->s_qcop->quota_on(sb, type, id, path);
}

static int quota_getfmt(struct super_block *sb, int type, void __user *addr)
{
	__u32 fmt;

	mutex_lock(&sb_dqopt(sb)->dqonoff_mutex);
	if (!sb_has_quota_active(sb, type)) {
		mutex_unlock(&sb_dqopt(sb)->dqonoff_mutex);
		return -ESRCH;
	}
	fmt = sb_dqopt(sb)->info[type].dqi_format->qf_fmt_id;
	mutex_unlock(&sb_dqopt(sb)->dqonoff_mutex);
	if (copy_to_user(addr, &fmt, sizeof(fmt)))
		return -EFAULT;
	return 0;
}

static int quota_getinfo(struct super_block *sb, int type, void __user *addr)
{
	struct if_dqinfo info;
	int ret;

	if (!sb->s_qcop->get_info)
		return -ENOSYS;
	ret = sb->s_qcop->get_info(sb, type, &info);
	if (!ret && copy_to_user(addr, &info, sizeof(info)))
		return -EFAULT;
	return ret;
}

static int quota_setinfo(struct super_block *sb, int type, void __user *addr)
{
	struct if_dqinfo info;

	if (copy_from_user(&info, addr, sizeof(info)))
		return -EFAULT;
	if (!sb->s_qcop->set_info)
		return -ENOSYS;
	return sb->s_qcop->set_info(sb, type, &info);
}

static void copy_to_if_dqblk(struct if_dqblk *dst, struct fs_disk_quota *src)
{
	dst->dqb_bhardlimit = src->d_blk_hardlimit;
	dst->dqb_bsoftlimit = src->d_blk_softlimit;
	dst->dqb_curspace = src->d_bcount;
	dst->dqb_ihardlimit = src->d_ino_hardlimit;
	dst->dqb_isoftlimit = src->d_ino_softlimit;
	dst->dqb_curinodes = src->d_icount;
	dst->dqb_btime = src->d_btimer;
	dst->dqb_itime = src->d_itimer;
	dst->dqb_valid = QIF_ALL;
}

static int quota_getquota(struct super_block *sb, int type, qid_t id,
			  void __user *addr)
{
	struct kqid qid;
	struct fs_disk_quota fdq;
	struct if_dqblk idq;
	int ret;

	if (!sb->s_qcop->get_dqblk)
		return -ENOSYS;
	qid = make_kqid(current_user_ns(), type, id);
	if (!qid_valid(qid))
		return -EINVAL;
	ret = sb->s_qcop->get_dqblk(sb, qid, &fdq);
	if (ret)
		return ret;
	copy_to_if_dqblk(&idq, &fdq);
	if (copy_to_user(addr, &idq, sizeof(idq)))
		return -EFAULT;
	return 0;
}

static void copy_from_if_dqblk(struct fs_disk_quota *dst, struct if_dqblk *src)
{
	dst->d_blk_hardlimit = src->dqb_bhardlimit;
	dst->d_blk_softlimit  = src->dqb_bsoftlimit;
	dst->d_bcount = src->dqb_curspace;
	dst->d_ino_hardlimit = src->dqb_ihardlimit;
	dst->d_ino_softlimit = src->dqb_isoftlimit;
	dst->d_icount = src->dqb_curinodes;
	dst->d_btimer = src->dqb_btime;
	dst->d_itimer = src->dqb_itime;

	dst->d_fieldmask = 0;
	if (src->dqb_valid & QIF_BLIMITS)
		dst->d_fieldmask |= FS_DQ_BSOFT | FS_DQ_BHARD;
	if (src->dqb_valid & QIF_SPACE)
		dst->d_fieldmask |= FS_DQ_BCOUNT;
	if (src->dqb_valid & QIF_ILIMITS)
		dst->d_fieldmask |= FS_DQ_ISOFT | FS_DQ_IHARD;
	if (src->dqb_valid & QIF_INODES)
		dst->d_fieldmask |= FS_DQ_ICOUNT;
	if (src->dqb_valid & QIF_BTIME)
		dst->d_fieldmask |= FS_DQ_BTIMER;
	if (src->dqb_valid & QIF_ITIME)
		dst->d_fieldmask |= FS_DQ_ITIMER;
}

static int quota_setquota(struct super_block *sb, int type, qid_t id,
			  void __user *addr)
{
	struct fs_disk_quota fdq;
	struct if_dqblk idq;
	struct kqid qid;

	if (copy_from_user(&idq, addr, sizeof(idq)))
		return -EFAULT;
	if (!sb->s_qcop->set_dqblk)
		return -ENOSYS;
	qid = make_kqid(current_user_ns(), type, id);
	if (!qid_valid(qid))
		return -EINVAL;
	copy_from_if_dqblk(&fdq, &idq);
	return sb->s_qcop->set_dqblk(sb, qid, &fdq);
}

static int quota_setxstate(struct super_block *sb, int cmd, void __user *addr)
{
	__u32 flags;

	if (copy_from_user(&flags, addr, sizeof(flags)))
		return -EFAULT;
	if (!sb->s_qcop->set_xstate)
		return -ENOSYS;
	return sb->s_qcop->set_xstate(sb, flags, cmd);
}

static int quota_getxstate(struct super_block *sb, void __user *addr)
{
	struct fs_quota_stat fqs;
	int ret;

	if (!sb->s_qcop->get_xstate)
		return -ENOSYS;
	ret = sb->s_qcop->get_xstate(sb, &fqs);
	if (!ret && copy_to_user(addr, &fqs, sizeof(fqs)))
		return -EFAULT;
	return ret;
}

static int quota_getxstatev(struct super_block *sb, void __user *addr)
{
	struct fs_quota_statv fqs;
	int ret;

	if (!sb->s_qcop->get_xstatev)
		return -ENOSYS;

	memset(&fqs, 0, sizeof(fqs));
	if (copy_from_user(&fqs, addr, 1)) /* Just read qs_version */
		return -EFAULT;

	/* If this kernel doesn't support user specified version, fail */
	switch (fqs.qs_version) {
	case FS_QSTATV_VERSION1:
		break;
	default:
		return -EINVAL;
	}
	ret = sb->s_qcop->get_xstatev(sb, &fqs);
	if (!ret && copy_to_user(addr, &fqs, sizeof(fqs)))
		return -EFAULT;
	return ret;
}

static int quota_setxquota(struct super_block *sb, int type, qid_t id,
			   void __user *addr)
{
	struct fs_disk_quota fdq;
	struct kqid qid;

	if (copy_from_user(&fdq, addr, sizeof(fdq)))
		return -EFAULT;
	if (!sb->s_qcop->set_dqblk)
		return -ENOSYS;
	qid = make_kqid(current_user_ns(), type, id);
	if (!qid_valid(qid))
		return -EINVAL;
	return sb->s_qcop->set_dqblk(sb, qid, &fdq);
}

static int quota_getxquota(struct super_block *sb, int type, qid_t id,
			   void __user *addr)
{
	struct fs_disk_quota fdq;
	struct kqid qid;
	int ret;

	if (!sb->s_qcop->get_dqblk)
		return -ENOSYS;
	qid = make_kqid(current_user_ns(), type, id);
	if (!qid_valid(qid))
		return -EINVAL;
	ret = sb->s_qcop->get_dqblk(sb, qid, &fdq);
	if (!ret && copy_to_user(addr, &fdq, sizeof(fdq)))
		return -EFAULT;
	return ret;
}

static int quota_rmxquota(struct super_block *sb, void __user *addr)
{
	__u32 flags;

	if (copy_from_user(&flags, addr, sizeof(flags)))
		return -EFAULT;
	if (!sb_has_rm_xquota(sb) || !sb->s_qcop->rm_xquota)
		return -ENOSYS;
	return sb->s_qcop->rm_xquota(sb, flags);
}

/* Copy parameters and call proper function */
static int do_quotactl(struct super_block *sb, int type, int cmd, qid_t id,
		       void __user *addr, struct path *path)
{
	int ret;

	if (type >= (XQM_COMMAND(cmd) ? XQM_MAXQUOTAS : MAXQUOTAS))
		return -EINVAL;
	if (!sb->s_qcop)
		return -ENOSYS;

	ret = check_quotactl_permission(sb, type, cmd, id);
	if (ret < 0)
		return ret;

	switch (cmd) {
	case Q_QUOTAON:
		return quota_quotaon(sb, type, cmd, id, path);
	case Q_QUOTAOFF:
		if (!sb->s_qcop->quota_off)
			return -ENOSYS;
		return sb->s_qcop->quota_off(sb, type);
	case Q_GETFMT:
		return quota_getfmt(sb, type, addr);
	case Q_GETINFO:
		return quota_getinfo(sb, type, addr);
	case Q_SETINFO:
		return quota_setinfo(sb, type, addr);
	case Q_GETQUOTA:
		return quota_getquota(sb, type, id, addr);
	case Q_SETQUOTA:
		return quota_setquota(sb, type, id, addr);
	case Q_SYNC:
		if (!sb->s_qcop->quota_sync)
			return -ENOSYS;
		return sb->s_qcop->quota_sync(sb, type);
	case Q_XQUOTAON:
	case Q_XQUOTAOFF:
		return quota_setxstate(sb, cmd, addr);
	case Q_XQUOTARM:
		return quota_rmxquota(sb, addr);
	case Q_XGETQSTAT:
		return quota_getxstate(sb, addr);
	case Q_XGETQSTATV:
		return quota_getxstatev(sb, addr);
	case Q_XSETQLIM:
		return quota_setxquota(sb, type, id, addr);
	case Q_XGETQUOTA:
		return quota_getxquota(sb, type, id, addr);
	case Q_XQUOTASYNC:
		if (sb->s_flags & MS_RDONLY)
			return -EROFS;
		/* XFS quotas are fully coherent now, making this call a noop */
		return 0;
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_BLOCK

/* Return 1 if 'cmd' will block on frozen filesystem */
static int quotactl_cmd_write(int cmd)
{
	switch (cmd) {
	case Q_GETFMT:
	case Q_GETINFO:
	case Q_SYNC:
	case Q_XGETQSTAT:
	case Q_XGETQSTATV:
	case Q_XGETQUOTA:
	case Q_XQUOTASYNC:
		return 0;
	}
	return 1;
}

#endif /* CONFIG_BLOCK */

/*
 * look up a superblock on which quota ops will be performed
 * - use the name of a block device to find the superblock thereon
 */
static struct super_block *quotactl_block(const char __user *special, int cmd)
{
#ifdef CONFIG_BLOCK
	struct block_device *bdev;
	struct super_block *sb;
	struct filename *tmp = getname(special);

	if (IS_ERR(tmp))
		return ERR_CAST(tmp);
	bdev = lookup_bdev(tmp->name);
	putname(tmp);
	if (IS_ERR(bdev))
		return ERR_CAST(bdev);
	if (quotactl_cmd_write(cmd))
		sb = get_super_thawed(bdev);
	else
		sb = get_super(bdev);
	bdput(bdev);
	if (!sb)
		return ERR_PTR(-ENODEV);

	return sb;
#else
	return ERR_PTR(-ENODEV);
#endif
}

#ifdef CONFIG_QUOTA_COMPAT

asmlinkage long sys_quotactl(unsigned int cmd, const char __user *special, qid_t id, void __user *addr);

static long compat_quotactl(unsigned int cmds, unsigned int type,
		const char __user *special, qid_t id,
		void __user *addr)
{
	struct super_block *sb;
	long ret;

	sb = NULL;
	switch (cmds) {
		case QC_QUOTAON:
			return sys_quotactl(QCMD(Q_QUOTAON, type),
					special, id, addr);

		case QC_QUOTAOFF:
			return sys_quotactl(QCMD(Q_QUOTAOFF, type),
					special, id, addr);

		case QC_SYNC:
			return sys_quotactl(QCMD(Q_SYNC, type),
					special, id, addr);

		case QC_GETQUOTA: {
			struct if_dqblk idq;
			struct fs_disk_quota fdq;
			struct compat_dqblk cdq;
			struct kqid qid;

			sb = quotactl_block(special, cmds);
			ret = PTR_ERR(sb);
			if (IS_ERR(sb))
				break;
			ret = check_quotactl_permission(sb, type, Q_GETQUOTA, id);
			if (ret)
				break;
			qid = make_kqid(current_user_ns(), type, id);
			ret = -EINVAL;
			if (!qid_valid(qid))
				break;
			ret = sb->s_qcop->get_dqblk(sb, qid, &fdq);
			copy_to_if_dqblk(&idq, &fdq);
			if (ret)
				break;
			cdq.dqb_ihardlimit = fdq.d_ino_hardlimit;
			cdq.dqb_isoftlimit = fdq.d_ino_softlimit;
			cdq.dqb_curinodes = fdq.d_icount;
			cdq.dqb_bhardlimit = fdq.d_blk_hardlimit;
			cdq.dqb_bsoftlimit = fdq.d_blk_softlimit;
			cdq.dqb_curspace = fdq.d_bcount;
			cdq.dqb_btime = fdq.d_btimer;
			cdq.dqb_itime = fdq.d_itimer;
			ret = 0;
			if (copy_to_user(addr, &cdq, sizeof(cdq)))
				ret = -EFAULT;
			break;
		}

		case QC_SETQUOTA:
		case QC_SETUSE:
		case QC_SETQLIM: {
			struct if_dqblk idq;
			struct fs_disk_quota fdq;
			struct compat_dqblk cdq;
			struct kqid qid;

			sb = quotactl_block(special, cmds);
			ret = PTR_ERR(sb);
			if (IS_ERR(sb))
				break;
			ret = check_quotactl_permission(sb, type, Q_GETQUOTA, id);
			if (ret)
				break;
			ret = -EFAULT;
			if (copy_from_user(&cdq, addr, sizeof(cdq)))
				break;
			qid = make_kqid(current_user_ns(), type, id);
			ret = -EINVAL;
			if (!qid_valid(qid))
				break;
			idq.dqb_ihardlimit = cdq.dqb_ihardlimit;
			idq.dqb_isoftlimit = cdq.dqb_isoftlimit;
			idq.dqb_curinodes = cdq.dqb_curinodes;
			idq.dqb_bhardlimit = cdq.dqb_bhardlimit;
			idq.dqb_bsoftlimit = cdq.dqb_bsoftlimit;
			idq.dqb_curspace = cdq.dqb_curspace;
			idq.dqb_valid = 0;
			if (cmds == QC_SETQUOTA || cmds == QC_SETQLIM)
				idq.dqb_valid |= QIF_LIMITS;
			if (cmds == QC_SETQUOTA || cmds == QC_SETUSE)
				idq.dqb_valid |= QIF_USAGE;
			copy_from_if_dqblk(&fdq, &idq);
			ret = sb->s_qcop->set_dqblk(sb, qid, &fdq);
			break;
		}

		case QC_GETINFO: {
			struct if_dqinfo iinf;
			struct compat_dqinfo cinf;

			sb = quotactl_block(special, cmds);
			ret = PTR_ERR(sb);
			if (IS_ERR(sb))
				break;
			ret = check_quotactl_permission(sb, type, Q_GETQUOTA, id);
			if (ret)
				break;
			ret = sb->s_qcop->get_info(sb, type, &iinf);
			if (ret)
				break;
			cinf.dqi_bgrace = iinf.dqi_bgrace;
			cinf.dqi_igrace = iinf.dqi_igrace;
			cinf.dqi_flags = 0;
			if (iinf.dqi_flags & DQF_INFO_DIRTY)
				cinf.dqi_flags |= 0x0010;
			cinf.dqi_blocks = 0;
			cinf.dqi_free_blk = 0;
			cinf.dqi_free_entry = 0;
			ret = 0;
			if (copy_to_user(addr, &cinf, sizeof(cinf)))
				ret = -EFAULT;
			break;
		}

		case QC_SETINFO:
		case QC_SETGRACE:
		case QC_SETFLAGS: {
			struct if_dqinfo iinf;
			struct compat_dqinfo cinf;

			sb = quotactl_block(special, cmds);
			ret = PTR_ERR(sb);
			if (IS_ERR(sb))
				break;
			ret = check_quotactl_permission(sb, type, Q_GETQUOTA, id);
			if (ret)
				break;
			ret = -EFAULT;
			if (copy_from_user(&cinf, addr, sizeof(cinf)))
				break;
			iinf.dqi_bgrace = cinf.dqi_bgrace;
			iinf.dqi_igrace = cinf.dqi_igrace;
			iinf.dqi_flags = cinf.dqi_flags;
			iinf.dqi_valid = 0;
			if (cmds == QC_SETINFO || cmds == QC_SETGRACE)
				iinf.dqi_valid |= IIF_BGRACE | IIF_IGRACE;
			if (cmds == QC_SETINFO || cmds == QC_SETFLAGS)
				iinf.dqi_valid |= IIF_FLAGS;
			ret = sb->s_qcop->set_info(sb, type, &iinf);
			break;
		}

		case QC_GETSTATS: {
			struct compat_dqstats stat;

			memset(&stat, 0, sizeof(stat));
			stat.version = 6*10000+5*100+0;
			ret = 0;
			if (copy_to_user(addr, &stat, sizeof(stat)))
				ret = -EFAULT;
			break;
		}

		default:
			ret = -ENOSYS;
			break;
	}
	if (sb && !IS_ERR(sb))
		drop_super(sb);
	return ret;
}

#endif

/*
 * This is the system call interface. This communicates with
 * the user-level programs. Currently this only supports diskquota
 * calls. Maybe we need to add the process quotas etc. in the future,
 * but we probably should use rlimits for that.
 */
SYSCALL_DEFINE4(quotactl, unsigned int, cmd, const char __user *, special,
		qid_t, id, void __user *, addr)
{
	uint cmds, type;
	struct super_block *sb = NULL;
	struct path path, *pathp = NULL;
	int ret;

	cmds = cmd >> SUBCMDSHIFT;
	type = cmd & SUBCMDMASK;

#ifdef CONFIG_QUOTA_COMPAT
	if (cmds >= 0x0100 && cmds < 0x3000)
		return compat_quotactl(cmds, type, special, id, addr);
#endif

	/*
	 * As a special case Q_SYNC can be called without a specific device.
	 * It will iterate all superblocks that have quota enabled and call
	 * the sync action on each of them.
	 */
	if (!special) {
		if (cmds == Q_SYNC)
			return quota_sync_all(type);
		return -ENODEV;
	}

	/*
	 * Path for quotaon has to be resolved before grabbing superblock
	 * because that gets s_umount sem which is also possibly needed by path
	 * resolution (think about autofs) and thus deadlocks could arise.
	 */
	if (cmds == Q_QUOTAON) {
		ret = user_path_at(AT_FDCWD, addr, LOOKUP_FOLLOW|LOOKUP_AUTOMOUNT, &path);
		if (ret)
			pathp = ERR_PTR(ret);
		else
			pathp = &path;
	}

	sb = quotactl_block(special, cmds);
	if (IS_ERR(sb)) {
		ret = PTR_ERR(sb);
		goto out;
	}

	ret = do_quotactl(sb, type, cmds, id, addr, pathp);

	drop_super(sb);
out:
	if (pathp && !IS_ERR(pathp))
		path_put(pathp);
	return ret;
}