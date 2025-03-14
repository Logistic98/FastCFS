/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_iov.h"
#include "fcfs_api_util.h"
#include "async_reporter.h"
#include "fcfs_api_file.h"

#define FCFS_API_MAGIC_NUMBER    1588076578

static int file_truncate(FCFSAPIContext *ctx, const int64_t oid,
        const int64_t new_size, const FDIRDentryOperator *oper,
        const int64_t tid);

static int deal_open_flags(FCFSAPIFileInfo *fi, FDIRClientOperFnamePair *path,
        const FDIRDentryOperator *oper, const mode_t mode,
        const int64_t tid, int result)
{
    int bytes;

    fi->tid = tid;
    fi->oper = *oper;
    if (fi->ctx->persist_additional_gids) {
        /* persist fi->oper.additional_gids.list for future usage */
        if (oper->additional_gids.count > 0) {
            bytes = FDIR_ADDITIONAL_GROUP_BYTES(*oper);
            if (bytes <= sizeof(fi->fixed_groups_buff)) {
                fi->oper.additional_gids.list = fi->fixed_groups_buff;
            } else {
                fi->oper.additional_gids.list = fc_malloc(bytes);
                if (fi->oper.additional_gids.list == NULL) {
                    return ENOMEM;
                }
            }
            memcpy((void *)fi->oper.additional_gids.list,
                    oper->additional_gids.list, bytes);
        } else {
            fi->oper.additional_gids.list = fi->fixed_groups_buff;
        }
    }

    if (!((fi->flags & O_WRONLY) || (fi->flags & O_RDWR))) {
        fi->offset = 0;
        return (fi->flags & O_TRUNC) ? EACCES : result;
    }

    if ((fi->flags & O_CREAT)) {
        if (result == 0) {
            if ((fi->flags & O_EXCL)) {
                return EEXIST;
            }
        } else if (result == ENOENT) {
            if ((result=fdir_client_create_dentry(fi->ctx->contexts.fdir,
                            path, mode, &fi->dentry)) != 0)
            {
                if (result == EEXIST) {
                    if ((fi->flags & O_EXCL)) {
                        return EEXIST;
                    }

                    if ((result=fcfs_api_access_dentry_by_path_ex(fi->ctx,
                                    path, FCFS_API_GET_ACCESS_MASK(fi->flags),
                                    FCFS_API_GET_ACCESS_FLAGS(fi->flags),
                                    &fi->dentry)) != 0)
                    {
                        return result;
                    }
                } else {
                    return result;
                }
            }
        } else {
            return result;
        }
    } else if (result != 0) {
        return result;
    }

    if (S_ISDIR(fi->dentry.stat.mode)) {
        return EISDIR;  //flags O_WRONLY or O_RDWR are forbidden for directory
    }
    if ((fi->flags & O_NOFOLLOW) && S_ISLNK(fi->dentry.stat.mode)) {
        return ELOOP;
    }

    fi->write_notify.last_modified_time = fi->dentry.stat.mtime;
    if ((fi->flags & O_TRUNC)) {
        if (fi->dentry.stat.size > 0) {
            if (S_ISLNK(fi->dentry.stat.mode)) {
                return EPERM;
            }
            if ((result=file_truncate(fi->ctx, fi->dentry.
                            inode, 0, &fi->oper, tid)) != 0)
            {
                return result;
            }

            fi->dentry.stat.size = 0;
            fi->dentry.stat.space_end = 0;
        }
    }

    if ((fi->flags & O_APPEND)) {
        fi->offset = fi->dentry.stat.size;
    } else {
        fi->offset = 0;
    }
    return 0;
}

#define SET_FILE_COMMON_FIELDS(fi, _ctx, _flags) \
    fi->ctx = _ctx;     \
    fi->flags = _flags; \
    fi->sessions.flock.mconn = NULL

int fcfs_api_open_ex(FCFSAPIContext *ctx, FCFSAPIFileInfo *fi,
        const char *path, const int flags,
        const FCFSAPIFileContext *fctx)
{
    FDIRClientOperFnamePair fname;
    mode_t mode;
    int result;

    if ((fctx->mode & S_IFMT)) {
        mode = (fctx->mode & (~S_IFMT)) | S_IFREG;
    } else {
        mode = fctx->mode | S_IFREG;
    }

    SET_FILE_COMMON_FIELDS(fi, ctx, flags);
    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, fctx->oper, path);
    result = fcfs_api_access_dentry_by_path_ex(ctx,
            &fname, FCFS_API_GET_ACCESS_MASK(flags),
            FCFS_API_GET_ACCESS_FLAGS(flags), &fi->dentry);
    if ((result=deal_open_flags(fi, &fname, &fctx->oper,
                    mode, fctx->tid, result)) != 0)
    {
        return result;
    }

    fi->magic = FCFS_API_MAGIC_NUMBER;
    return 0;
}

int fcfs_api_open_by_dentry_ex(FCFSAPIContext *ctx, FCFSAPIFileInfo *fi,
        const FDIRDEntryInfo *dentry, const int flags,
        const FCFSAPIFileContext *fctx)
{
    int result;

    SET_FILE_COMMON_FIELDS(fi, ctx, flags);
    fi->dentry = *dentry;
    result = 0;
    if ((result=deal_open_flags(fi, NULL, &fctx->oper,
                    fctx->mode, fctx->tid, result)) != 0)
    {
        return result;
    }

    fi->magic = FCFS_API_MAGIC_NUMBER;
    return 0;
}

int fcfs_api_open_by_inode_ex(FCFSAPIContext *ctx, FCFSAPIFileInfo *fi,
        const int64_t inode, const int flags, const FCFSAPIFileContext *fctx)
{
    FDIRClientOperInodePair oino;
    int result;

    FCFSAPI_SET_OPER_INODE_PAIR(oino, fctx->oper, inode);
    if ((result=fcfs_api_access_dentry_by_inode_ex(ctx, &oino,
                    FCFS_API_GET_ACCESS_MASK(flags),
                    FCFS_API_GET_ACCESS_FLAGS(flags),
                    &fi->dentry)) != 0)
    {
        return result;
    }

    SET_FILE_COMMON_FIELDS(fi, ctx, flags);
    if ((result=deal_open_flags(fi, NULL, &fctx->oper,
                    fctx->mode, fctx->tid, result)) != 0)
    {
        return result;
    }

    fi->magic = FCFS_API_MAGIC_NUMBER;
    return 0;
}

int fcfs_api_close(FCFSAPIFileInfo *fi)
{
    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if (fi->sessions.flock.mconn != NULL) {
        /* force close connection to unlock */
        fdir_client_close_session(&fi->sessions.flock, true);
    }

    if (fi->ctx->persist_additional_gids) {
        if (fi->oper.additional_gids.list != fi->fixed_groups_buff) {
            free((void *)fi->oper.additional_gids.list);
            fi->oper.additional_gids.list = NULL;
        }
    }

    fi->ctx = NULL;
    fi->magic = 0;
    return 0;
}

static int report_size_and_time(FCFSAPIContext *ctx,
        const FDIRSetDEntrySizeInfo *dsize, FDIRDEntryInfo *dentry)
{
    if (ctx->async_report.enabled) {
        if (dentry != NULL) {
            if ((dsize->flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE)) {
                dentry->stat.size = dsize->file_size;
            }
            if ((dsize->flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END)) {
                dentry->stat.space_end = dsize->file_size;
            }
            if ((dsize->flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_MTIME)) {
                dentry->stat.mtime = get_current_time();
            }
        }
        return async_reporter_push(dsize);
    } else {
        FDIRDEntryInfo tmp;
        if (dentry == NULL) {
            dentry = &tmp;
        }
        return fdir_client_set_dentry_size(ctx->contexts.fdir,
                &ctx->ns, dsize, dentry);
    }
}

static int fcfs_api_file_report_size_and_time(
        FCFSAPIWriteDoneCallbackArg *callback_arg, int *flags,
        FDIRDEntryInfo *dentry)
{
    FDIRSetDEntrySizeInfo dsize;
    int current_time;

    dsize.file_size = callback_arg->arg.bs_key->block.offset +
        callback_arg->arg.bs_key->slice.offset +
        callback_arg->arg.write_bytes;
    if (dsize.file_size > callback_arg->extra.file_size) {
        *flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
            FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
    } else {
        if (dsize.file_size > callback_arg->extra.space_end) {
            *flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        } else {
            *flags = 0;
        }

        current_time = get_current_time();
        if (current_time > callback_arg->extra.last_modified_time) {
            *flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_MTIME;
        }
    }

    if (callback_arg->arg.inc_alloc != 0)  {
        if (!(*flags & (FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
                        FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END)))
        {
            *flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END |
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_INC_ALLOC;
        } else {
            *flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_INC_ALLOC;
        }
    }

    if (*flags == 0) {
        return 0;
    } else {
        dsize.inode = callback_arg->arg.bs_key->block.oid;
        dsize.inc_alloc = callback_arg->arg.inc_alloc;
        dsize.force = false;
        dsize.flags = *flags;
        return report_size_and_time(callback_arg->extra.ctx, &dsize, dentry);
    }
}

void fcfs_api_file_write_done_callback(FSAPIWriteDoneCallbackArg *callback_arg)
{
    int flags;
    fcfs_api_file_report_size_and_time((FCFSAPIWriteDoneCallbackArg *)
            callback_arg, &flags, NULL);
}

/*
static inline void print_block_slice_key(FSBlockSliceKeyInfo *bs_key)
{
    logInfo("block {oid: %"PRId64", offset: %"PRId64"}, "
            "slice {offset: %d, length: %d}", bs_key->block.oid,
            bs_key->block.offset, bs_key->slice.offset, bs_key->slice.length);
}
*/

static int do_pwrite(FCFSAPIFileInfo *fi, FSAPIWriteBuffer *wbuffer,
        const int size, const int64_t offset, int *written_bytes,
        int *total_inc_alloc, const bool need_report_modified,
        const int64_t tid)
{
    FSAPIOperationContext op_ctx;
    FCFSAPIWriteDoneCallbackArg callback_arg;
    SFDynamicIOVArray iova;
    int64_t new_offset;
    int loop_flags;
    int result;
    int remain;

    FS_API_SET_CTX_AND_TID_EX(op_ctx, fi->ctx->contexts.fsapi, tid);
    wbuffer->extra_data = &callback_arg.extra;
    callback_arg.extra.ctx = fi->ctx;

    *total_inc_alloc = *written_bytes = 0;
    new_offset = offset;
    fs_set_block_slice(&op_ctx.bs_key, fi->dentry.inode, offset, size);

    loop_flags = 1;
    if (wbuffer->is_writev) {
        sf_iova_init(iova, wbuffer->iov, wbuffer->iovcnt);
        if (op_ctx.bs_key.slice.length < size) {
            if ((result=sf_iova_first_slice(&iova, op_ctx.
                            bs_key.slice.length)) != 0)
            {
                loop_flags = 0;
            }
        }
    }

    while (loop_flags) {
        //print_block_slice_key(&op_ctx.bs_key);
        callback_arg.arg.bs_key = &op_ctx.bs_key;
        callback_arg.extra.file_size = fi->dentry.stat.size;
        callback_arg.extra.space_end = fi->dentry.stat.space_end;
        callback_arg.extra.last_modified_time = fi->write_notify.last_modified_time;
        if ((result=fs_api_slice_write(&op_ctx, wbuffer, &callback_arg.
                        arg.write_bytes, &callback_arg.arg.inc_alloc)) != 0)
        {
            if (callback_arg.arg.write_bytes == 0) {
                break;
            }
        }

        new_offset += callback_arg.arg.write_bytes;
        *written_bytes += callback_arg.arg.write_bytes;
        *total_inc_alloc += callback_arg.arg.inc_alloc;
        if (need_report_modified) {
            int flags;
            fcfs_api_file_report_size_and_time(&callback_arg,
                    &flags, &fi->dentry);
            if ((flags & FDIR_DENTRY_FIELD_MODIFIED_FLAG_MTIME) != 0) {
                fi->write_notify.last_modified_time = get_current_time();
            }
        }

        remain = size - *written_bytes;
        if (remain <= 0) {
            break;
        }

        if (callback_arg.arg.write_bytes == op_ctx.bs_key.slice.length) {
            /* fully completed */
            fs_next_block_slice_key(&op_ctx.bs_key, remain);
        } else {  //partially completed, try again the remain part
            fs_set_slice_size(&op_ctx.bs_key, new_offset, remain);
        }

        if (wbuffer->is_writev) {
            if ((result=sf_iova_next_slice(&iova, *written_bytes,
                            op_ctx.bs_key.slice.length)) != 0)
            {
                break;
            }
        } else {
            wbuffer->buff += *written_bytes;
        }
    }

    if (wbuffer->is_writev) {
        sf_iova_destroy(iova);
    }
    return (*written_bytes > 0) ? 0 : EIO;
}

static inline int check_writable(FCFSAPIFileInfo *fi)
{
    if (fi->magic != FCFS_API_MAGIC_NUMBER || !((fi->flags & O_WRONLY) ||
                (fi->flags & O_RDWR)))
    {
        return EBADF;
    }

    if (S_ISDIR(fi->dentry.stat.mode)) {
        return EISDIR;
    }
    return (S_ISREG(fi->dentry.stat.mode) ? 0 : EPERM);
}

static inline int check_readable(FCFSAPIFileInfo *fi)
{
    if (fi->magic != FCFS_API_MAGIC_NUMBER || (fi->flags & O_WRONLY)) {
        return EBADF;
    }

    if (S_ISDIR(fi->dentry.stat.mode)) {
        return EISDIR;
    }
    return (S_ISREG(fi->dentry.stat.mode) ? 0 : EPERM);
}

static int pwrite_wrapper(FCFSAPIFileInfo *fi, FSAPIWriteBuffer *wbuffer,
        const int size, const int64_t offset, int *written_bytes,
        const int64_t tid)
{
    int total_inc_alloc;
    int result;

    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if ((result=check_writable(fi)) != 0) {
        return result;
    }

    return do_pwrite(fi, wbuffer, size, offset, written_bytes,
            &total_inc_alloc, true, tid);
}

int fcfs_api_pwrite_ex(FCFSAPIFileInfo *fi, const char *buff,
        const int size, const int64_t offset, int *written_bytes,
        const int64_t tid)
{
    FSAPIWriteBuffer wbuffer;

    FS_API_SET_WBUFFER_BUFF(wbuffer, buff);
    return pwrite_wrapper(fi, &wbuffer, size, offset, written_bytes, tid);
}

static int do_write(FCFSAPIFileInfo *fi, FSAPIWriteBuffer *wbuffer,
        const int size, int *written_bytes, const int64_t tid)
{
    FDIRClientSession session;
    FDIRSetDEntrySizeInfo dsize;
    bool use_sys_lock;
    int result;
    int total_inc_alloc;
    int64_t old_size;
    int64_t space_end;

    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if ((result=check_writable(fi)) != 0) {
        return result;
    }

    use_sys_lock = fi->ctx->use_sys_lock_for_append &&
        !fi->ctx->async_report.enabled && (fi->flags & O_APPEND);
    if (use_sys_lock) {
        if ((result=fcfs_api_dentry_sys_lock(&session, fi->dentry.inode,
                        0, &old_size, &space_end)) != 0)
        {
            return result;
        }

        fi->offset = old_size;
    } else {
        old_size = fi->dentry.stat.size;
    }

    if ((result=do_pwrite(fi, wbuffer, size, fi->offset, written_bytes,
                    &total_inc_alloc, !use_sys_lock, tid)) == 0)
    {
        fi->offset += *written_bytes;
    }

    if (use_sys_lock) {
        string_t *ns;
        if (fi->offset > old_size) {
            ns = &fi->ctx->ns; //set ns for update file_size, space_end etc.
        } else {
            ns = NULL;  //do NOT update
        }

        dsize.inode = fi->dentry.inode;
        dsize.file_size = fi->offset;
        dsize.inc_alloc = total_inc_alloc;
        dsize.force = false;
        dsize.flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
            FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        fcfs_api_dentry_sys_unlock(&session, ns, old_size, &dsize);
    }

    return result;
}

int fcfs_api_write_ex(FCFSAPIFileInfo *fi, const char *buff,
        const int size, int *written_bytes, const int64_t tid)
{
    FSAPIWriteBuffer wbuffer;

    FS_API_SET_WBUFFER_BUFF(wbuffer, buff);
    return do_write(fi, &wbuffer, size, written_bytes, tid);
}

int fcfs_api_pwritev_ex(FCFSAPIFileInfo *fi, const struct iovec *iov,
        const int iovcnt, const int64_t offset, int *written_bytes,
        const int64_t tid)
{
    FSAPIWriteBuffer wbuffer;

    FS_API_SET_WBUFFER_IOV(wbuffer, iov, iovcnt);
    return pwrite_wrapper(fi, &wbuffer, fc_iov_get_bytes(iov, iovcnt),
            offset, written_bytes, tid);
}

int fcfs_api_writev_ex(FCFSAPIFileInfo *fi, const struct iovec *iov,
        const int iovcnt, int *written_bytes, const int64_t tid)
{
    FSAPIWriteBuffer wbuffer;

    FS_API_SET_WBUFFER_IOV(wbuffer, iov, iovcnt);
    return do_write(fi, &wbuffer, fc_iov_get_bytes(iov, iovcnt),
            written_bytes, tid);
}

static int do_pread(FCFSAPIFileInfo *fi, const bool is_readv,
        const void *data, const int iovcnt, const int size,
        const int64_t offset, int *read_bytes, const int64_t tid)
{
    const int flags = FDIR_FLAGS_FOLLOW_SYMLINK;
    FSAPIOperationContext op_ctx;
    SFDynamicIOVArray iova;
    FDIRClientOperInodePair oino;
    int result;
    int current_read;
    int remain;
    int64_t current_offset;
    int64_t hole_bytes;
    int fill_bytes;

    *read_bytes = 0;
    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if ((result=check_readable(fi)) != 0) {
        return result;
    }

    FS_API_SET_CTX_AND_TID_EX(op_ctx, fi->ctx->contexts.fsapi, tid);
    fs_set_block_slice(&op_ctx.bs_key, fi->dentry.inode, offset, size);
    if (is_readv) {
        sf_iova_init(iova, (struct iovec *)data, iovcnt);
    }
    while (1) {
        //print_block_slice_key(&op_ctx.bs_key);
        if (is_readv) {
            result = fs_api_slice_readv(&op_ctx, iova.iov,
                    iova.cnt, &current_read);
        } else {
            result = fs_api_slice_read(&op_ctx, (char *)data +
                    (*read_bytes), &current_read);
        }

        if (result != 0) {
            if (result == ENODATA) {
                result = 0;
            } else {
                break;
            }
        }

        while (current_read < op_ctx.bs_key.slice.length) {
            /* deal file hole caused by ftruncate and lseek */
            current_offset = offset + *read_bytes + current_read;
            if (current_offset == fi->dentry.stat.size) {
                break;
            }

            if (current_offset > fi->dentry.stat.size) {
                FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
                if ((result=fcfs_api_stat_dentry_by_inode_ex(fi->ctx,
                                &oino, flags, &fi->dentry)) != 0)
                {
                    break;
                }
            }

            hole_bytes = fi->dentry.stat.size - current_offset;
            if (hole_bytes > 0) {
                if ((int64_t)current_read + hole_bytes > (int64_t)
                        op_ctx.bs_key.slice.length)
                {
                    fill_bytes = op_ctx.bs_key.slice.length - current_read;
                } else {
                    fill_bytes = hole_bytes;
                }

                /*
                logInfo("=====offset: %"PRId64", current_read: %d, "
                        "hole_bytes: %"PRId64", fill_bytes: %d, "
                        "buff offset: %d =====", offset, current_read,
                        hole_bytes, fill_bytes, *read_bytes + current_read);
                        */

                if (is_readv) {
                    if ((result=sf_iova_memset(iova, 0, current_read,
                                    fill_bytes)) != 0)
                    {
                        break;
                    }
                } else {
                    memset((char *)data + (*read_bytes) +
                            current_read, 0, fill_bytes);
                }
                current_read += fill_bytes;
            }

            break;
        }

        /*
        logInfo("current read: %d, total read: %d, slice length: %d",
                current_read, *read_bytes, op_ctx.bs_key.slice.length);
                */

        *read_bytes += current_read;
        remain = size - *read_bytes;
        if (remain <= 0 || current_read < op_ctx.bs_key.slice.length) {
            break;
        }

        if (is_readv) {
            if ((result=sf_iova_consume(&iova, current_read)) != 0) {
                break;
            }
        }
        fs_next_block_slice_key(&op_ctx.bs_key, remain);
    }

    if (is_readv) {
        sf_iova_destroy(iova);
    }
    return result;
}

int fcfs_api_pread_ex(FCFSAPIFileInfo *fi, char *buff, const int size,
        const int64_t offset, int *read_bytes, const int64_t tid)
{
    const bool is_readv = false;
    const int iovcnt = 0;

    return do_pread(fi, is_readv, buff, iovcnt,
            size, offset, read_bytes, tid);
}

int fcfs_api_read_ex(FCFSAPIFileInfo *fi, char *buff, const int size,
        int *read_bytes, const int64_t tid)
{
    const bool is_readv = false;
    const int iovcnt = 0;
    int result;

    if ((result=do_pread(fi, is_readv, buff, iovcnt, size,
                    fi->offset, read_bytes, tid)) != 0)
    {
        return result;
    }

    fi->offset += *read_bytes;
    return 0;
}

int fcfs_api_preadv_ex(FCFSAPIFileInfo *fi, const struct iovec *iov,
        const int iovcnt, const int64_t offset, int *read_bytes,
        const int64_t tid)
{
    const bool is_readv = true;

    return do_pread(fi, is_readv, iov, iovcnt,
            fc_iov_get_bytes(iov, iovcnt),
            offset, read_bytes, tid);
}

int fcfs_api_readv_ex(FCFSAPIFileInfo *fi, const struct iovec *iov,
        const int iovcnt, int *read_bytes, const int64_t tid)
{
    const bool is_readv = true;
    int result;

    if ((result=do_pread(fi, is_readv, iov, iovcnt,
                    fc_iov_get_bytes(iov, iovcnt),
                    fi->offset, read_bytes, tid)) != 0)
    {
        return result;
    }

    fi->offset += *read_bytes;
    return 0;
}

static int do_truncate(FCFSAPIContext *ctx, const int64_t oid,
        const int64_t old_space_end, const int64_t offset,
        const int64_t length, int64_t *total_dec_alloc, const int64_t tid)
{
    FSAPIOperationContext op_ctx;
    int64_t remain;
    int dec_alloc;
    int result;

    *total_dec_alloc = 0;
    if (offset >= old_space_end) {
        return 0;
    }

    if (offset + length > old_space_end) {
        remain = old_space_end - offset;
    } else {
        remain = length;
    }

    FS_API_SET_CTX_AND_TID_EX(op_ctx, ctx->contexts.fsapi, tid);
    fs_set_block_slice(&op_ctx.bs_key, oid, offset, remain);
    while (1) {
        //print_block_slice_key(&op_ctx.bs_key);
        if (op_ctx.bs_key.slice.length == FS_FILE_BLOCK_SIZE) {
            result = fs_api_block_delete(&op_ctx,
                    &dec_alloc);
        } else {
            result = fs_api_slice_delete(&op_ctx,
                    &dec_alloc);
        }
        if (result == 0) {
            *total_dec_alloc -= dec_alloc;
        } else if (result == ENOENT) {
            result = 0;
        } else if (result != 0) {
            break;
        }

        remain -= op_ctx.bs_key.slice.length;
        if (remain <= 0) {
            break;
        }

        fs_next_block_slice_key(&op_ctx.bs_key, remain);
    }

    return result;
}

static int do_allocate(FCFSAPIContext *ctx, const int64_t oid,
        const int64_t offset, const int64_t length,
        int64_t *total_inc_alloc, const int64_t tid)
{
    FSAPIOperationContext op_ctx;
    int64_t remain;
    int inc_alloc;
    int result;

    *total_inc_alloc = 0;
    if (length <= 0) {
        return 0;
    }

    FS_API_SET_CTX_AND_TID_EX(op_ctx, ctx->contexts.fsapi, tid);
    remain = length;
    fs_set_block_slice(&op_ctx.bs_key, oid, offset, remain);
    while (1) {
        //print_block_slice_key(&op_ctx.bs_key);
        if ((result=fs_api_slice_allocate(&op_ctx, &inc_alloc)) != 0) {
            break;
        }
        *total_inc_alloc += inc_alloc;

        remain -= op_ctx.bs_key.slice.length;
        if (remain <= 0) {
            break;
        }

        fs_next_block_slice_key(&op_ctx.bs_key, remain);
    }

    return result;
}

static inline int check_and_sys_lock(FCFSAPIContext *ctx,
        FDIRClientSession *session, const int64_t oid,
        const FDIRDentryOperator *oper,
        int64_t *old_size, int64_t *space_end)
{
    const int flags = FDIR_FLAGS_FOLLOW_SYMLINK;
    FDIRClientOperInodePair oino;
    FDIRDEntryInfo dentry;
    int result;

    if (ctx->use_sys_lock_for_append && !ctx->async_report.enabled) {
        result = fcfs_api_dentry_sys_lock(session,
                oid, 0, old_size, space_end);
    } else {
        FCFSAPI_SET_OPER_INODE_PAIR(oino, *oper, oid);
        if ((result=fcfs_api_stat_dentry_by_inode_ex(ctx,
                        &oino, flags, &dentry)) == 0)
        {
            *old_size = dentry.stat.size;
            *space_end = dentry.stat.space_end;
        }
    }

    return result;
}

static inline int check_and_sys_unlock(FCFSAPIContext *ctx,
        FDIRClientSession *session, const int64_t old_size,
        const FDIRSetDEntrySizeInfo *dsize, const int result)
{
    if (ctx->use_sys_lock_for_append && !ctx->async_report.enabled) {
        string_t *ns;
        int unlock_res;

        if (result == 0) {
            ns = &ctx->ns;  //set ns for update file_size, space_end etc.
        } else {
            ns = NULL;  //do NOT update
        }
        unlock_res = fcfs_api_dentry_sys_unlock(session, ns, old_size, dsize);
        return result == 0 ? unlock_res : result;
    } else {
        if (result == 0) {
            return report_size_and_time(ctx, dsize, NULL);
        } else {
            return result;
        }
    }
}

static int file_truncate(FCFSAPIContext *ctx, const int64_t oid,
        const int64_t new_size, const FDIRDentryOperator *oper,
        const int64_t tid)
{
    FDIRClientSession session;
    FDIRSetDEntrySizeInfo dsize;
    int64_t old_size;
    int64_t space_end;
    int result;

    if (new_size < 0) {
        return EINVAL;
    }

    if ((result=check_and_sys_lock(ctx, &session, oid,
                    oper, &old_size, &space_end)) != 0)
    {
        return result;
    }

    dsize.inode = oid;
    dsize.file_size = new_size;
    dsize.force = true;
    if (new_size >= old_size) {
        result = 0;
        dsize.inc_alloc = 0;
        if (new_size == old_size) {
            dsize.flags = 0;
        } else {
            dsize.flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        }
    } else {
        result = do_truncate(ctx, oid, space_end, new_size,
                old_size - new_size, &dsize.inc_alloc, tid);

        dsize.flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        if (new_size < space_end) {
            dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        }
        if (dsize.inc_alloc != 0)  {
            dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_INC_ALLOC;
        }
    }

    return check_and_sys_unlock(ctx, &session, old_size, &dsize, result);
}

int fcfs_api_ftruncate_ex(FCFSAPIFileInfo *fi, const int64_t new_size,
        const int64_t tid)
{
    int result;

    if ((result=check_writable(fi)) != 0) {
        return result;
    }

    return file_truncate(fi->ctx, fi->dentry.inode, new_size,
            &fi->oper, tid);
}

static int get_regular_file_inode(FCFSAPIContext *ctx,
        const FDIRClientOperFnamePair *fname, int64_t *inode)
{
    const int flags = FDIR_FLAGS_FOLLOW_SYMLINK;
    FDIRDEntryInfo dentry;
    int result;

    if ((result=fcfs_api_stat_dentry_by_fullname_ex(ctx, fname,
                    flags, LOG_DEBUG, &dentry)) != 0)
    {
        return result;
    }
    if (S_ISDIR(dentry.stat.mode)) {
        return EISDIR;
    }

    *inode = dentry.inode;
    return 0;
}

int fcfs_api_truncate_ex(FCFSAPIContext *ctx, const char *path,
        const int64_t new_size, const FCFSAPIFileContext *fctx)
{
    int result;
    FDIRClientOperFnamePair fname;
    int64_t inode;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, fctx->oper, path);
    if ((result=get_regular_file_inode(ctx, &fname, &inode)) != 0) {
        return result;
    }

    return file_truncate(ctx, inode, new_size, &fname.oper, fctx->tid);
}

int fcfs_api_file_truncate_ex(FCFSAPIContext *ctx,
       const FDIRClientOperInodePair *oino,
       const int64_t new_size, const int64_t tid,
       FDIRDEntryInfo *dentry)
{
    const int flags = FDIR_FLAGS_OUTPUT_DENTRY;
    int result;

    if ((result=fcfs_api_access_dentry_by_inode_ex(ctx,
                    oino, W_OK, flags, dentry)) != 0)
    {
        return result;
    }
    if (S_ISDIR(dentry->stat.mode)) {
        return EISDIR;
    }

    return file_truncate(ctx, oino->inode, new_size, &oino->oper, tid);
}

#define calc_file_offset(fi, offset, whence, new_offset)  \
    calc_file_offset_ex(fi, offset, whence, false, new_offset)

static inline int calc_file_offset_ex(FCFSAPIFileInfo *fi,
        const int64_t offset, const int whence,
        const bool refresh_fsize, int64_t *new_offset)
{
    const int flags = FDIR_FLAGS_FOLLOW_SYMLINK;
    int result;
    FDIRClientOperInodePair oino;

    switch (whence) {
        case SEEK_SET:
            if (offset < 0) {
                return EINVAL;
            }
            *new_offset = offset;
            break;
        case SEEK_CUR:
            *new_offset = fi->offset + offset;
            if (*new_offset < 0) {
                return EINVAL;
            }
            break;
        case SEEK_END:
            if (refresh_fsize) {
                FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
                if ((result=fcfs_api_stat_dentry_by_inode_ex(fi->ctx,
                                &oino, flags, &fi->dentry)) != 0)
                {
                    return result;
                }
            }

            *new_offset = fi->dentry.stat.size + offset;
            if (*new_offset < 0) {
                return EINVAL;
            }
            break;
        default:
            logError("file: "__FILE__", line: %d, "
                    "invalid whence: %d", __LINE__, whence);
            return EINVAL;
    }

    return 0;
}

int fcfs_api_lseek(FCFSAPIFileInfo *fi, const int64_t offset, const int whence)
{
    int64_t new_offset;
    int result;

    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=calc_file_offset_ex(fi, offset, whence,
                    true, &new_offset)) != 0)
    {
        return result;
    }

    fi->offset = new_offset;
    return 0;
}

void fcfs_api_fill_stat(const FDIRDEntryInfo *dentry, struct stat *stat)
{
    stat->st_ino = dentry->inode;
    stat->st_rdev = dentry->stat.rdev;
    stat->st_mode = dentry->stat.mode;
    stat->st_size = dentry->stat.size;
    stat->st_atime = (uint32_t)dentry->stat.atime;
    stat->st_mtime = (uint32_t)dentry->stat.mtime;
    stat->st_ctime = (uint32_t)dentry->stat.ctime;
    stat->st_uid = dentry->stat.uid;
    stat->st_gid = dentry->stat.gid;
    stat->st_nlink = dentry->stat.nlink;

    stat->st_blksize = 512;
    if (dentry->stat.alloc > 0) {
        stat->st_blocks = (dentry->stat.alloc + stat->st_blksize - 1) /
            stat->st_blksize;
    }
}

int fcfs_api_fstat(FCFSAPIFileInfo *fi, struct stat *buf)
{
    const int flags = FDIR_FLAGS_FOLLOW_SYMLINK;
    int result;
    FDIRClientOperInodePair oino;

    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
    if ((result=fcfs_api_stat_dentry_by_inode_ex(fi->ctx,
                    &oino, flags, &fi->dentry)) != 0)
    {
        return result;
    }

    memset(buf, 0, sizeof(struct stat));
    fcfs_api_fill_stat(&fi->dentry, buf);
    return 0;
}

static inline int fapi_stat(FCFSAPIContext *ctx,
        const FDIRClientOperFnamePair *path,
        struct stat *buf, const int flags)
{
    int result;
    FDIRDEntryInfo dentry;

    if ((result=fcfs_api_stat_dentry_by_fullname_ex(ctx,
                    path, flags, LOG_DEBUG, &dentry)) != 0)
    {
        return result;
    }

    memset(buf, 0, sizeof(struct stat));
    fcfs_api_fill_stat(&dentry, buf);
    return 0;
}

int fcfs_api_lstat_ex(FCFSAPIContext *ctx, const char *path,
        const FDIRDentryOperator *oper, struct stat *buf)
{
    const int flags = 0;
    FDIRClientOperFnamePair fname;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fapi_stat(ctx, &fname, buf, flags);
}

int fcfs_api_stat_ex(FCFSAPIContext *ctx, const char *path,
        const FDIRDentryOperator *oper, struct stat *buf,
        const int flags)
{
    FDIRClientOperFnamePair fname;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fapi_stat(ctx, &fname, buf, flags);
}

static inline int fcntl_lock(FCFSAPIFileInfo *fi, const int operation,
        const int64_t offset, const int64_t length,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    FDIRFlockOwner owner;
    FDIRClientOperInodePair oino;

    if (fi->sessions.flock.mconn == NULL) {
        if ((result=fdir_client_init_session(fi->ctx->contexts.
                        fdir, &fi->sessions.flock)) != 0)
        {
            return result;
        }
    }

    FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
    owner.id = owner_id;
    owner.pid = pid;
    if ((result=fdir_client_flock_dentry_ex(&fi->sessions.flock,
                    &fi->ctx->ns, &oino, operation,
                    offset, length, &owner)) != 0)
    {
        return result;
    }

    return 0;
}

static inline int fcntl_unlock(FCFSAPIFileInfo *fi, const int operation,
        const int64_t offset, const int64_t length,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    FDIRFlockOwner owner;
    FDIRClientOperInodePair oino;

    if (fi->sessions.flock.mconn == NULL) {
        return 0;
    }

    FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
    owner.id = owner_id;
    owner.pid = pid;
    if ((result=fdir_client_flock_dentry_ex(&fi->sessions.flock,
            &fi->ctx->ns, &oino, operation,
            offset, length, &owner)) != 0)
    {
        return (result == ENOENT ? 0 : result);
    }

    return 0;
}

int fcfs_api_flock_ex2(FCFSAPIFileInfo *fi, const int operation,
        const int64_t owner_id, const pid_t pid)
{
    const int64_t offset = 0;
    const int64_t length = 0;

    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((operation & LOCK_UN)) {
        return fcntl_unlock(fi, operation, offset, length, owner_id, pid);
    } else if ((operation & LOCK_SH) || (operation & LOCK_EX)) {
        return fcntl_lock(fi, operation, offset, length, owner_id, pid);
    } else {
        return EINVAL;
    }
}

static inline int fcntl_type_to_flock_op(const short type, int *operation)
{
    switch (type) {
        case F_RDLCK:
            *operation = LOCK_SH;
            break;
        case F_WRLCK:
            *operation = LOCK_EX;
            break;
        case F_UNLCK:
            *operation = LOCK_UN;
            break;
        default:
            return EINVAL;
    }

    return 0;
}

static inline int flock_op_to_fcntl_type(const int operation, short *type)
{
    switch (operation) {
        case LOCK_SH:
            *type = F_RDLCK;
            break;
        case LOCK_EX:
            *type = F_WRLCK;
            break;
        case LOCK_UN:
            *type = F_UNLCK;
            break;
        default:
            return EINVAL;
    }

    return 0;
}

int fcfs_api_setlk_ex(FCFSAPIFileInfo *fi, const struct flock *lock,
        const int64_t owner_id, const bool blocked)
{
    int operation;
    int result;
    int64_t offset;

    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=fcntl_type_to_flock_op(lock->l_type, &operation)) != 0) {
        return result;
    }

    if ((result=calc_file_offset(fi, lock->l_start,
                    lock->l_whence, &offset)) != 0)
    {
        return result;
    }

    if (operation == LOCK_UN) {
        return fcntl_unlock(fi, operation, offset,
                lock->l_len, owner_id, lock->l_pid);
    } else {
        if (!blocked) {
            operation |= LOCK_NB;
        }
        return fcntl_lock(fi, operation, offset, lock->l_len,
                owner_id, lock->l_pid);
    }
}

int fcfs_api_getlk_ex(FCFSAPIFileInfo *fi,
        struct flock *lock, int64_t *owner_id)
{
    int operation;
    int result;
    int64_t offset;
    int64_t length;
    FDIRClientOperInodePair oino;
    FDIRFlockOwner owner;

    if (fi->magic != FCFS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=fcntl_type_to_flock_op(lock->l_type, &operation)) != 0) {
        return result;
    }
    if (operation == LOCK_UN) {
        return EINVAL;
    }

    if ((result=calc_file_offset(fi, lock->l_start,
                    lock->l_whence, &offset)) != 0)
    {
        return result;
    }

    FCFSAPI_SET_OPER_INODE_PAIR(oino, fi->oper, fi->dentry.inode);
    length = lock->l_len;
    owner.id = *owner_id;
    owner.pid = lock->l_pid;
    if ((result=fdir_client_getlk_dentry(fi->ctx->contexts.fdir,
                    &fi->ctx->ns, &oino, &operation,
                    &offset, &length, &owner)) == 0)
    {
        flock_op_to_fcntl_type(operation, &lock->l_type);
        lock->l_whence = SEEK_SET;
        lock->l_start = offset;
        lock->l_len = length;
    }

    return result;
}

int fcfs_api_fallocate_ex(FCFSAPIFileInfo *fi, const int mode,
        const int64_t offset, const int64_t length, const int64_t tid)
{
    FDIRClientSession session;
    FDIRSetDEntrySizeInfo dsize;
    int64_t old_size;
    int64_t space_end;
    int op;
    int result;

    if (offset < 0 || length < 0) {
        return EINVAL;
    }

    if ((result=check_writable(fi)) != 0) {
        return result;
    }

    op = mode & (~FALLOC_FL_KEEP_SIZE);
    if (!(op == 0 || op == FALLOC_FL_PUNCH_HOLE)) {
        return EOPNOTSUPP;
    }

    if (length == 0) {
        return 0;
    }

    if (fi->ctx->use_sys_lock_for_append && !fi->ctx->async_report.enabled) {
        if ((result=fcfs_api_dentry_sys_lock(&session, fi->dentry.
                        inode, 0, &old_size, &space_end)) != 0)
        {
            return result;
        }
    } else {
        old_size = fi->dentry.stat.size;
        space_end = fi->dentry.stat.space_end;
    }

    dsize.inode = fi->dentry.inode;
    dsize.force = true;
    dsize.flags = 0;
    if (op == 0) {   //allocate space
        result = do_allocate(fi->ctx, fi->dentry.inode,
                offset, length, &dsize.inc_alloc, tid);
        dsize.file_size = offset + length;
        if (dsize.file_size > space_end) {
            dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        }
        if (dsize.file_size > old_size) {
            dsize.flags |= (mode & FALLOC_FL_KEEP_SIZE) ? 0 :
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        }
    } else {  //deallocate space
        result = do_truncate(fi->ctx, fi->dentry.inode, space_end,
                offset, length, &dsize.inc_alloc, tid);
        if (offset + length >= old_size) {
            dsize.file_size = offset;
            dsize.flags |= (mode & FALLOC_FL_KEEP_SIZE) ? 0 :
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
            if (dsize.file_size < space_end) {
                dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
            }
        } else {
            if (offset < space_end && offset + length >= space_end) {
                dsize.file_size = offset;
                dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
            } else {
                dsize.file_size = old_size;
            }
        }
    }
    if (dsize.inc_alloc != 0)  {
        dsize.flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_INC_ALLOC;
    }
    return check_and_sys_unlock(fi->ctx, &session, old_size, &dsize, result);
}

int fcfs_api_rename_ex(FCFSAPIContext *ctx, const char *old_path,
        const char *new_path, const int flags,
        const FCFSAPIFileContext *fctx)
{
    FDIRDEntryFullName src_fullname;
    FDIRDEntryFullName dest_fullname;
    FDIRDEntryInfo dentry;
    FDIRDEntryInfo *pe;
    int result;

    src_fullname.ns = ctx->ns;
    FC_SET_STRING(src_fullname.path, (char *)old_path);
    dest_fullname.ns = ctx->ns;
    FC_SET_STRING(dest_fullname.path, (char *)new_path);

    pe = &dentry;
    if ((result=fdir_client_rename_dentry_ex(ctx->contexts.fdir,
                    &src_fullname, &dest_fullname, &fctx->oper,
                    flags, &pe)) != 0)
    {
        return result;
    }

    if (pe != NULL && S_ISREG(pe->stat.mode) && pe->stat.nlink == 0
            && !ctx->contexts.fdir->trash_bin_enabled)
    {
        fs_api_unlink_file(ctx->contexts.fsapi, pe->inode,
                pe->stat.space_end, fctx->tid);
    }

    return result;
}

int fcfs_api_symlink_ex(FCFSAPIContext *ctx, const char *target,
        const char *path, const FDIRDentryOperator *oper,
        const mode_t mode)
{
    FDIRClientOperFnamePair fname;
    string_t link;
    FDIRDEntryInfo dentry;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    FC_SET_STRING(link, (char *)target);
    return fdir_client_symlink_dentry(ctx->contexts.fdir,
            &link, &fname, mode, &dentry);
}

int fcfs_api_readlink(FCFSAPIContext *ctx, const char *path,
        const FDIRDentryOperator *oper, char *buff, const int size)
{
    FDIRClientOperFnamePair fname;
    string_t link;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    link.str = buff;
    return fdir_client_readlink_by_path(ctx->contexts.fdir,
        &fname, &link, size);
}

int fcfs_api_link_ex(FCFSAPIContext *ctx, const char *old_path,
        const char *new_path, const FDIRDentryOperator *oper,
        const mode_t mode, const int flags)
{
    FDIRDEntryFullName src_fullname;
    FDIRDEntryFullName dest_fullname;
    FDIRDEntryInfo dentry;

    src_fullname.ns = ctx->ns;
    FC_SET_STRING(src_fullname.path, (char *)old_path);
    dest_fullname.ns = ctx->ns;
    FC_SET_STRING(dest_fullname.path, (char *)new_path);

    return fdir_client_link_dentry(ctx->contexts.fdir, &src_fullname,
            &dest_fullname, oper, mode, flags, &dentry);
}

int fcfs_api_mknod_ex(FCFSAPIContext *ctx, const char *path,
        const FDIRDentryOperator *oper, const mode_t mode,
        const dev_t dev)
{
    FDIRClientOperFnamePair fname;
    FDIRDEntryInfo dentry;

    if (!(S_ISCHR(mode) || S_ISBLK(mode))) {
        return EINVAL;
    }

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fdir_client_create_dentry_ex(ctx->contexts.fdir,
            &fname, mode, dev, &dentry);
}

static inline int do_make_dentry(FCFSAPIContext *ctx, const char *path,
        FDIRDentryOperator *oper, const mode_t mode, const int mtype)
{
    FDIRClientOperFnamePair fname;
    FDIRDEntryInfo dentry;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fdir_client_create_dentry(ctx->contexts.fdir,
            &fname, ((mode & (~S_IFMT)) | mtype), &dentry);
}

int fcfs_api_mkfifo_ex(FCFSAPIContext *ctx, const char *path,
        FDIRDentryOperator *oper, const mode_t mode)
{
    return do_make_dentry(ctx, path, oper, mode, S_IFIFO);
}

int fcfs_api_mkdir_ex(FCFSAPIContext *ctx, const char *path,
        FDIRDentryOperator *oper, const mode_t mode)
{
    return do_make_dentry(ctx, path, oper, mode, S_IFDIR);
}

int fcfs_api_statvfs_ex(FCFSAPIContext *ctx, const char *path,
        struct statvfs *stbuf)
{
    int result;
    FCFSAuthClientFullContext *auth;
    int64_t quota;
    int64_t total;
    int64_t avail;
    FDIRClientNamespaceStat nstat;

    if (ctx->contexts.fdir->auth.enabled) {
        auth = &ctx->contexts.fdir->auth;
    } else if (ctx->contexts.fsapi->fs->auth.enabled) {
        auth = &ctx->contexts.fsapi->fs->auth;
    } else {
        auth = NULL;
    }

    if (auth != NULL) {
        if ((result=fcfs_auth_client_spool_get_quota(
                        auth->ctx, &ctx->ns, &quota)) != 0)
        {
            return result;
        }
    } else {
        quota = FCFS_AUTH_UNLIMITED_QUOTA_VAL;
    }

    if ((result=fdir_client_namespace_stat(ctx->contexts.
                    fdir, &ctx->ns, &nstat)) != 0)
    {
        return result;
    }

    if (quota == FCFS_AUTH_UNLIMITED_QUOTA_VAL) {
        FSClusterSpaceStat sstat;
        if ((result=fs_api_cluster_space_stat(ctx->
                        contexts.fsapi, &sstat)) != 0)
        {
            return result;
        }

        total = sstat.total;
        avail = total - sstat.used;
    } else {
        total = quota;
        avail = total - nstat.space.used;
    }
    if (avail < 0) {
        avail = 0;
    }

    stbuf->f_bsize = stbuf->f_frsize = 512;
    stbuf->f_blocks = total / stbuf->f_frsize;
    stbuf->f_bavail = stbuf->f_bfree = avail / stbuf->f_frsize;

    stbuf->f_files = nstat.inode.total;
    stbuf->f_ffree = nstat.inode.total - nstat.inode.used;
    stbuf->f_favail = nstat.inode.avail;

    stbuf->f_namemax = NAME_MAX;
    stbuf->f_fsid = 0;
    stbuf->f_flag = 0;
    return 0;
}

int fcfs_api_access_ex(FCFSAPIContext *ctx, const char *path,
        const int mask, const FDIRDentryOperator *oper,
        const int flags)
{
    FDIRClientOperFnamePair fname;
    FDIRDEntryInfo dentry;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fcfs_api_access_dentry_by_path_ex(ctx,
            &fname, mask, flags, &dentry);
}

int fcfs_api_euidaccess_ex(FCFSAPIContext *ctx, const char *path,
        const int mask, const FDIRDentryOperator *oper,
        const int flags)
{
    FDIRClientOperFnamePair fname;
    FDIRDEntryInfo dentry;

    FCFSAPI_SET_PATH_OPER_FNAME(fname, ctx, *oper, path);
    return fcfs_api_access_dentry_by_path_ex(ctx,
            &fname, mask, flags, &dentry);
}

int fcfs_api_set_file_flags(FCFSAPIFileInfo *fi, const int flags)
{
    /*
    if (flags & (O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK)) {
        logInfo("set flags: %d, is O_DIRECT: %d, is O_NOATIME: %d, "
                "is O_NONBLOCK: %d", flags, (flags & O_DIRECT),
                (flags & O_NOATIME), (flags & O_NONBLOCK));
    }
    */

    if ((flags & O_APPEND) == 0) {
        return 0;
    }

    if (!((fi->flags & O_WRONLY) || (fi->flags & O_RDWR))) {
        return EBADF;
    }

    fi->flags |= O_APPEND;
    fi->offset = fi->dentry.stat.size;
    return 0;
}
