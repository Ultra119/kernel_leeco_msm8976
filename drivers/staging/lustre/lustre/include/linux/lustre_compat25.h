/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef _LINUX_COMPAT25_H
#define _LINUX_COMPAT25_H

#include <linux/fs_struct.h>
#include <linux/namei.h>
#include <linux/libcfs/linux/portals_compat25.h>

#include <linux/lustre_patchless_compat.h>

# define LOCK_FS_STRUCT(fs)	spin_lock(&(fs)->lock)
# define UNLOCK_FS_STRUCT(fs)	spin_unlock(&(fs)->lock)

static inline void ll_set_fs_pwd(struct fs_struct *fs, struct vfsmount *mnt,
				 struct dentry *dentry)
{
	struct path path;
	struct path old_pwd;

	path.mnt = mnt;
	path.dentry = dentry;
	LOCK_FS_STRUCT(fs);
	old_pwd = fs->pwd;
	path_get(&path);
	fs->pwd = path;
	UNLOCK_FS_STRUCT(fs);

	if (old_pwd.dentry)
		path_put(&old_pwd);
}


/*
 * set ATTR_BLOCKS to a high value to avoid any risk of collision with other
 * ATTR_* attributes (see bug 13828)
 */
#define ATTR_BLOCKS    (1 << 27)

#define current_ngroups current_cred()->group_info->ngroups
#define current_groups current_cred()->group_info->small_block

/*
 * OBD need working random driver, thus all our
 * initialization routines must be called after device
 * driver initialization
 */
#ifndef MODULE
#undef module_init
#define module_init(a)     late_initcall(a)
#endif


#define LTIME_S(time)		   (time.tv_sec)

#define ll_permission(inode,mask,nd)    inode_permission(inode,mask)

# define ll_generic_permission(inode, mask, flags, check_acl) \
	 generic_permission(inode, mask)

#define ll_blkdev_put(a, b) blkdev_put(a, b)

#define ll_dentry_open(a,b,c)	dentry_open(a,b,c)

#define ll_vfs_symlink(dir, dentry, mnt, path, mode) \
		       vfs_symlink(dir, dentry, path)


#define ll_generic_file_llseek_size(file, offset, origin, maxbytes, eof) \
		generic_file_llseek_size(file, offset, origin, maxbytes, eof);

/* inode_dio_wait(i) use as-is for write lock */
# define inode_dio_write_done(i)	do {} while (0) /* for write unlock */
# define inode_dio_read(i)		atomic_inc(&(i)->i_dio_count)
/* inode_dio_done(i) use as-is for read unlock */

#define TREE_READ_LOCK_IRQ(mapping)	spin_lock_irq(&(mapping)->tree_lock)
#define TREE_READ_UNLOCK_IRQ(mapping)	spin_unlock_irq(&(mapping)->tree_lock)

static inline
int ll_unregister_blkdev(unsigned int dev, const char *name)
{
	unregister_blkdev(dev, name);
	return 0;
}

#define ll_invalidate_bdev(a,b)	 invalidate_bdev((a))

#ifndef FS_HAS_FIEMAP
#define FS_HAS_FIEMAP			(0)
#endif



/* add a lustre compatible layer for crypto API */
#include <linux/crypto.h>
#define ll_crypto_hash	  crypto_hash
#define ll_crypto_cipher	crypto_blkcipher
#define ll_crypto_alloc_hash(name, type, mask)  crypto_alloc_hash(name, type, mask)
#define ll_crypto_hash_setkey(tfm, key, keylen) crypto_hash_setkey(tfm, key, keylen)
#define ll_crypto_hash_init(desc)	       crypto_hash_init(desc)
#define ll_crypto_hash_update(desc, sl, bytes)  crypto_hash_update(desc, sl, bytes)
#define ll_crypto_hash_final(desc, out)	 crypto_hash_final(desc, out)
#define ll_crypto_blkcipher_setkey(tfm, key, keylen) \
		crypto_blkcipher_setkey(tfm, key, keylen)
#define ll_crypto_blkcipher_set_iv(tfm, src, len) \
		crypto_blkcipher_set_iv(tfm, src, len)
#define ll_crypto_blkcipher_get_iv(tfm, dst, len) \
		crypto_blkcipher_get_iv(tfm, dst, len)
#define ll_crypto_blkcipher_encrypt(desc, dst, src, bytes) \
		crypto_blkcipher_encrypt(desc, dst, src, bytes)
#define ll_crypto_blkcipher_decrypt(desc, dst, src, bytes) \
		crypto_blkcipher_decrypt(desc, dst, src, bytes)
#define ll_crypto_blkcipher_encrypt_iv(desc, dst, src, bytes) \
		crypto_blkcipher_encrypt_iv(desc, dst, src, bytes)
#define ll_crypto_blkcipher_decrypt_iv(desc, dst, src, bytes) \
		crypto_blkcipher_decrypt_iv(desc, dst, src, bytes)

static inline
struct ll_crypto_cipher *ll_crypto_alloc_blkcipher(const char *name,
						   u32 type, u32 mask)
{
	struct ll_crypto_cipher *rtn = crypto_alloc_blkcipher(name, type, mask);

	return (rtn == NULL ? ERR_PTR(-ENOMEM) : rtn);
}

static inline int ll_crypto_hmac(struct ll_crypto_hash *tfm,
				 u8 *key, unsigned int *keylen,
				 struct scatterlist *sg,
				 unsigned int size, u8 *result)
{
	struct hash_desc desc;
	int	      rv;
	desc.tfm   = tfm;
	desc.flags = 0;
	rv = crypto_hash_setkey(desc.tfm, key, *keylen);
	if (rv) {
		CERROR("failed to hash setkey: %d\n", rv);
		return rv;
	}
	return crypto_hash_digest(&desc, sg, size, result);
}
static inline
unsigned int ll_crypto_tfm_alg_max_keysize(struct crypto_blkcipher *tfm)
{
	return crypto_blkcipher_tfm(tfm)->__crt_alg->cra_blkcipher.max_keysize;
}
static inline
unsigned int ll_crypto_tfm_alg_min_keysize(struct crypto_blkcipher *tfm)
{
	return crypto_blkcipher_tfm(tfm)->__crt_alg->cra_blkcipher.min_keysize;
}

#define ll_crypto_hash_blocksize(tfm)       crypto_hash_blocksize(tfm)
#define ll_crypto_hash_digestsize(tfm)      crypto_hash_digestsize(tfm)
#define ll_crypto_blkcipher_ivsize(tfm)     crypto_blkcipher_ivsize(tfm)
#define ll_crypto_blkcipher_blocksize(tfm)  crypto_blkcipher_blocksize(tfm)
#define ll_crypto_free_hash(tfm)	    crypto_free_hash(tfm)
#define ll_crypto_free_blkcipher(tfm)       crypto_free_blkcipher(tfm)

#define ll_vfs_rmdir(dir,entry,mnt)	     vfs_rmdir(dir,entry)
#define ll_vfs_mkdir(inode,dir,mnt,mode)	vfs_mkdir(inode,dir,mode)
#define ll_vfs_link(old,mnt,dir,new,mnt1)       vfs_link(old,dir,new)
#define ll_vfs_unlink(inode,entry,mnt)	  vfs_unlink(inode,entry)
#define ll_vfs_mknod(dir,entry,mnt,mode,dev)    vfs_mknod(dir,entry,mode,dev)
#define ll_security_inode_unlink(dir,entry,mnt) security_inode_unlink(dir,entry)
#define ll_vfs_rename(old,old_dir,mnt,new,new_dir,mnt1,delegated_inode) \
		vfs_rename(old,old_dir,new,new_dir,delegated_inode)

#ifdef for_each_possible_cpu
#define cfs_for_each_possible_cpu(cpu) for_each_possible_cpu(cpu)
#elif defined(for_each_cpu)
#define cfs_for_each_possible_cpu(cpu) for_each_cpu(cpu)
#endif

#define cfs_bio_io_error(a,b)   bio_io_error((a))
#define cfs_bio_endio(a,b,c)    bio_endio((a),(c))

#define cfs_fs_pwd(fs)       ((fs)->pwd.dentry)
#define cfs_fs_mnt(fs)       ((fs)->pwd.mnt)
#define cfs_path_put(nd)     path_put(&(nd)->path)


#ifndef SLAB_DESTROY_BY_RCU
#define SLAB_DESTROY_BY_RCU 0
#endif



static inline int
ll_quota_on(struct super_block *sb, int off, int ver, char *name, int remount)
{
	int rc;

	if (sb->s_qcop->quota_on) {
		struct path path;

		rc = kern_path(name, LOOKUP_FOLLOW, &path);
		if (!rc)
			return rc;
		rc = sb->s_qcop->quota_on(sb, off, ver
					    , &path
					   );
		path_put(&path);
		return rc;
	}
	else
		return -ENOSYS;
}

static inline int ll_quota_off(struct super_block *sb, int off, int remount)
{
	if (sb->s_qcop->quota_off) {
		return sb->s_qcop->quota_off(sb, off
					    );
	}
	else
		return -ENOSYS;
}


# define ll_vfs_dq_init	     dquot_initialize
# define ll_vfs_dq_drop	     dquot_drop
# define ll_vfs_dq_transfer	 dquot_transfer
# define ll_vfs_dq_off(sb, remount) dquot_suspend(sb, -1)





#define queue_max_phys_segments(rq)       queue_max_segments(rq)
#define queue_max_hw_segments(rq)	 queue_max_segments(rq)

#define ll_kmap_atomic(a, b)	kmap_atomic(a)
#define ll_kunmap_atomic(a, b)	kunmap_atomic(a)


#define ll_d_hlist_node hlist_node
#define ll_d_hlist_empty(list) hlist_empty(list)
#define ll_d_hlist_entry(ptr, type, name) hlist_entry(ptr.first, type, name)
#define ll_d_hlist_for_each(tmp, i_dentry) hlist_for_each(tmp, i_dentry)
#define ll_d_hlist_for_each_entry(dentry, p, i_dentry, alias) \
	p = NULL; hlist_for_each_entry(dentry, i_dentry, alias)


#define bio_hw_segments(q, bio) 0


#define ll_pagevec_init(pv, cold)       do {} while (0)
#define ll_pagevec_add(pv, pg)	  (0)
#define ll_pagevec_lru_add_file(pv)     do {} while (0)


#ifndef QUOTA_OK
# define QUOTA_OK 0
#endif
#ifndef NO_QUOTA
# define NO_QUOTA (-EDQUOT)
#endif

#ifndef SEEK_DATA
#define SEEK_DATA      3       /* seek to the next data */
#endif
#ifndef SEEK_HOLE
#define SEEK_HOLE      4       /* seek to the next hole */
#endif

#ifndef FMODE_UNSIGNED_OFFSET
#define FMODE_UNSIGNED_OFFSET	((__force fmode_t)0x2000)
#endif

#if !defined(_ASM_GENERIC_BITOPS_EXT2_NON_ATOMIC_H_) && !defined(ext2_set_bit)
# define ext2_set_bit	     __test_and_set_bit_le
# define ext2_clear_bit	   __test_and_clear_bit_le
# define ext2_test_bit	    test_bit_le
# define ext2_find_first_zero_bit find_first_zero_bit_le
# define ext2_find_next_zero_bit  find_next_zero_bit_le
#endif

#ifdef ATTR_TIMES_SET
# define TIMES_SET_FLAGS (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET)
#else
# define TIMES_SET_FLAGS (ATTR_MTIME_SET | ATTR_ATIME_SET)
#endif



/*
 * After 3.1, kernel's nameidata.intent.open.flags is different
 * with lustre's lookup_intent.it_flags, as lustre's it_flags'
 * lower bits equal to FMODE_xxx while kernel doesn't transliterate
 * lower bits of nameidata.intent.open.flags to FMODE_xxx.
 * */
#include <linux/version.h>
static inline int ll_namei_to_lookup_intent_flag(int flag)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 1, 0)
	flag = (flag & ~O_ACCMODE) | OPEN_FMODE(flag);
#endif
	return flag;
}

# define ll_mrf_ret void
# define LL_MRF_RETURN(rc)

#include <linux/fs.h>

# define ll_umode_t	umode_t

#include <linux/dcache.h>

# define ll_dirty_inode(inode, flag)	(inode)->i_sb->s_op->dirty_inode((inode), flag)

#endif /* _COMPAT25_H */
