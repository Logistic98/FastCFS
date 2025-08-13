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


#include <sys/stat.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "func.h"
#include "granted_pool.h"

int dao_granted_create(FDIRClientContext *client_ctx, const string_t *username,
        FCFSAuthGrantedPoolInfo *granted)
{
    int result;
    int64_t inode;
    AuthFullPath fp;
    FDIRClientOperFnamePair path;
    FDIRDEntryInfo dentry;

    AUTH_SET_GRANTED_POOL_PATH(fp, username, granted->pool_id);
    AUTH_SET_PATH_OPER_FNAME(path, fp);
    if ((result=fdir_client_lookup_inode_by_path_ex(client_ctx,
                    &path, LOG_DEBUG, &inode)) != 0)
    {
        if (result != ENOENT) {
            return result;
        }

        if ((result=fdir_client_create_dentry(client_ctx, &path,
                        DAO_MODE_FILE, &dentry)) != 0)
        {
            return result;
        }
        inode = dentry.inode;
    }

    if ((result=dao_set_xattr_integer(client_ctx, inode,
                    &AUTH_XTTR_NAME_FDIR, granted->privs.fdir)) != 0)
    {
        return result;
    }
    if ((result=dao_set_xattr_integer(client_ctx, inode,
                    &AUTH_XTTR_NAME_FSTORE, granted->privs.fstore)) != 0)
    {
        return result;
    }

    granted->id = inode;
    return 0;
}

int dao_granted_remove(FDIRClientContext *client_ctx,
        const string_t *username, const int64_t pool_id)
{
    const int flags = 0;
    AuthFullPath fp;
    FDIRClientOperFnamePair path;

    AUTH_SET_GRANTED_POOL_PATH(fp, username, pool_id);
    AUTH_SET_PATH_OPER_FNAME(path, fp);
    return fdir_client_remove_dentry(client_ctx, &path, flags);
}

static int dump_to_granted_array(FDIRClientContext *client_ctx,
        const FDIRClientDentryArray *darray,
        FCFSAuthGrantedPoolArray *parray)
{
    const FDIRClientDentry *entry;
    const FDIRClientDentry *end;
    char pool_id_buff[32];
    char *endptr;
    FCFSAuthGrantedPoolFullInfo *new_gpools;
    FCFSAuthGrantedPoolFullInfo *gpool;
    FCFSAuthGrantedPoolInfo *granted;
    int len;
    int result;

    if (darray->count > parray->alloc) {
        new_gpools = (FCFSAuthGrantedPoolFullInfo *)fc_malloc(
                sizeof(FCFSAuthGrantedPoolFullInfo) * darray->count);
        if (new_gpools == NULL) {
            return ENOMEM;
        }

        if (parray->gpools != parray->fixed) {
            free(parray->gpools);
        }
        parray->gpools = new_gpools;
        parray->alloc = darray->count;
    }

    end = darray->entries + darray->count;
    for (entry=darray->entries, gpool=parray->gpools;
            entry<end; entry++, gpool++)
    {
        if (entry->name.len < sizeof(pool_id_buff)) {
            len = entry->name.len;
        } else {
            len = sizeof(pool_id_buff) - 1;
        }
        memcpy(pool_id_buff, entry->name.str, len);
        *(pool_id_buff + len) = '\0';

        granted = &gpool->granted;
        granted->id = entry->dentry.inode;
        granted->pool_id = strtoll(pool_id_buff, &endptr, 10);
        if ((result=dao_get_xattr_int32(client_ctx, granted->id,
                        &AUTH_XTTR_NAME_FDIR, &granted->privs.fdir)) != 0)
        {
            return result;
        }
        if ((result=dao_get_xattr_int32(client_ctx, granted->id,
                        &AUTH_XTTR_NAME_FSTORE, &granted->privs.fstore)) != 0)
        {
            return result;
        }
    }

    parray->count = darray->count;
    return 0;
}

int dao_granted_list(FDIRClientContext *client_ctx, const string_t *username,
        FCFSAuthGrantedPoolArray *granted_array)
{
    const int flags = 0;
    int result;
    AuthFullPath fp;
    FDIRClientOperFnamePair path;
    FDIRClientDentryArray dentry_array;

    if ((result=fdir_client_dentry_array_init(&dentry_array)) != 0) {
        return result;
    }

    AUTH_SET_USER_PATH1(fp, username,
            AUTH_DIR_NAME_GRANTED_STR,
            AUTH_DIR_NAME_GRANTED_LEN);
    AUTH_SET_PATH_OPER_FNAME(path, fp);
    if ((result=fdir_client_list_dentry_by_path(client_ctx,
                    &path, &dentry_array, flags)) != 0)
    {
        fdir_client_dentry_array_free(&dentry_array);
        return result;
    }

    result = dump_to_granted_array(client_ctx,
            &dentry_array, granted_array);
    fdir_client_dentry_array_free(&dentry_array);
    return result;
}
