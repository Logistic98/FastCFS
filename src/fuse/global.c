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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include "fastcommon/sched_thread.h"
#include "fastcommon/system_info.h"
#include "sf/sf_global.h"
#include "sf/idempotency/client/client_channel.h"
#include "fuse_wrapper.h"
#include "global.h"

#define INI_FUSE_SECTION_NAME             "FUSE"
#define INI_GROUPS_CACHE_SECTION_NAME     "groups-cache"

#define FUSE_ALLOW_ALL_STR   "all"
#define FUSE_ALLOW_ROOT_STR  "root"

#define FUSE_MIN_SHARED_ALLOCATOR_COUNT             1
#define FUSE_MAX_SHARED_ALLOCATOR_COUNT           100
#define FUSE_DEFAULT_SHARED_ALLOCATOR_COUNT         7

#define FUSE_MIN_HASHTABLE_SHARDING_COUNT           1
#define FUSE_MAX_HASHTABLE_SHARDING_COUNT        1000
#define FUSE_DEFAULT_HASHTABLE_SHARDING_COUNT     163

#define FUSE_MIN_HASHTABLE_TOTAL_CAPACITY       10949
#define FUSE_MAX_HASHTABLE_TOTAL_CAPACITY     1000000
#define FUSE_DEFAULT_HASHTABLE_TOTAL_CAPACITY  175447

FUSEGlobalVars g_fuse_global_vars = {{NULL, NULL}};

static int load_fuse_config(IniFullContext *ini_ctx)
{
    string_t mountpoint;
    char *writeback_cache;
    char *allow_others;
    int result;

    get_kernel_version(&OS_KERNEL_VERSION);

    ini_ctx->section_name = INI_FUSE_SECTION_NAME;
    if (g_fuse_global_vars.nsmp.mountpoint == NULL) {
        FC_SET_STRING_NULL(mountpoint);
    } else {
        FC_SET_STRING(mountpoint, g_fuse_global_vars.nsmp.mountpoint);
    }
    if ((result=fcfs_api_load_ns_mountpoint(ini_ctx,
                    FCFS_API_DEFAULT_FASTDIR_SECTION_NAME,
                    &g_fuse_global_vars.nsmp, &mountpoint, true)) != 0)
    {
        return result;
    }

    g_fuse_global_vars.max_threads = iniGetIntCorrectValue(ini_ctx,
            "max_threads", 10, 1, 1024);
    g_fuse_global_vars.max_idle_threads = iniGetIntCorrectValue(ini_ctx,
            "max_idle_threads", g_fuse_global_vars.max_threads, 1, 1024);
    g_fuse_global_vars.singlethread = iniGetBoolValue(ini_ctx->
            section_name, "singlethread", ini_ctx->context, false);

    g_fuse_global_vars.clone_fd = iniGetBoolValue(ini_ctx->
            section_name, "clone_fd", ini_ctx->context, false);
    if (g_fuse_global_vars.clone_fd) {
        if (OS_KERNEL_VERSION.major < 4 || (OS_KERNEL_VERSION.major == 4 &&
            OS_KERNEL_VERSION.minor < 2))
        {
            logWarning("file: "__FILE__", line: %d, "
                    "kernel version %d.%d < 4.2, do NOT support "
                    "FUSE feature clone_fd", __LINE__,
                    OS_KERNEL_VERSION.major, OS_KERNEL_VERSION.minor);
            g_fuse_global_vars.clone_fd = false;
        }
    }

    g_fuse_global_vars.auto_unmount = iniGetBoolValue(ini_ctx->
            section_name, "auto_unmount", ini_ctx->context, false);
    g_fuse_global_vars.read_only = iniGetBoolValue(ini_ctx->
            section_name, "read_only", ini_ctx->context, false);

    allow_others = iniGetStrValue(ini_ctx->section_name,
            "allow_others", ini_ctx->context);
    if (allow_others == NULL || *allow_others == '\0') {
        g_fuse_global_vars.allow_others = allow_none;
    } else if (strcasecmp(allow_others, FUSE_ALLOW_ALL_STR) == 0) {
        g_fuse_global_vars.allow_others = allow_all;
    } else if (strcasecmp(allow_others, FUSE_ALLOW_ROOT_STR) == 0) {
        g_fuse_global_vars.allow_others = allow_root;
    } else {
        g_fuse_global_vars.allow_others = allow_none;
    }

    g_fuse_global_vars.attribute_timeout = iniGetDoubleValue(ini_ctx->
            section_name, "attribute_timeout", ini_ctx->context,
            FCFS_FUSE_DEFAULT_ATTRIBUTE_TIMEOUT);

    g_fuse_global_vars.entry_timeout = iniGetDoubleValue(ini_ctx->
            section_name, "entry_timeout", ini_ctx->context,
            FCFS_FUSE_DEFAULT_ENTRY_TIMEOUT);

    g_fuse_global_vars.xattr_enabled = iniGetBoolValue(ini_ctx->
            section_name, "xattr_enabled", ini_ctx->context, false);

    writeback_cache = iniGetStrValue(ini_ctx->section_name,
            "writeback_cache", ini_ctx->context);
    if (writeback_cache == NULL) {
        g_fuse_global_vars.writeback_cache = (OS_KERNEL_VERSION.major > 3 ||
                (OS_KERNEL_VERSION.major == 3 &&
                 OS_KERNEL_VERSION.minor >= 15));
    } else {
        g_fuse_global_vars.writeback_cache = FAST_INI_STRING_IS_TRUE(
                writeback_cache);
        if (g_fuse_global_vars.writeback_cache) {
            if (OS_KERNEL_VERSION.major < 3 || (OS_KERNEL_VERSION.major == 3 &&
                        OS_KERNEL_VERSION.minor < 15))
            {
                logWarning("file: "__FILE__", line: %d, "
                        "kernel version %d.%d < 3.15, do NOT support "
                        "FUSE feature writeback_cache", __LINE__,
                        OS_KERNEL_VERSION.major, OS_KERNEL_VERSION.minor);
                g_fuse_global_vars.writeback_cache = false;
            }
        }
    }

    g_fuse_global_vars.kernel_cache = iniGetBoolValue(ini_ctx->
            section_name, "kernel_cache", ini_ctx->context, true);
    ADDITIONAL_GROUPS_ENABLED = iniGetBoolValue(ini_ctx->section_name,
            "groups_enabled", ini_ctx->context, true);
    return 0;
}

static void load_additional_groups_config(IniFullContext *ini_ctx)
{
    ini_ctx->section_name = INI_GROUPS_CACHE_SECTION_NAME;
    GROUPS_CACHE_ENABLED = iniGetBoolValue(ini_ctx->
            section_name, "enabled", ini_ctx->context, true);

    GROUPS_CACHE_TIMEOUT = iniGetIntCorrectValue(ini_ctx,
            "timeout", 300, 1, 1e8);

    GROUPS_CACHE_ALLOCATOR_COUNT = iniGetIntCorrectValueEx(
            ini_ctx, "shared_allocator_count",
            FUSE_DEFAULT_SHARED_ALLOCATOR_COUNT,
            FUSE_MIN_SHARED_ALLOCATOR_COUNT,
            FUSE_MAX_SHARED_ALLOCATOR_COUNT, true);

    GROUPS_CACHE_SHARDING_COUNT = iniGetIntCorrectValue(
            ini_ctx, "hashtable_sharding_count",
            FUSE_DEFAULT_HASHTABLE_SHARDING_COUNT,
            FUSE_MIN_HASHTABLE_SHARDING_COUNT,
            FUSE_MAX_HASHTABLE_SHARDING_COUNT);

    GROUPS_CACHE_HTABLE_CAPACITY = iniGetIntCorrectValue(
            ini_ctx, "hashtable_total_capacity",
            FUSE_DEFAULT_HASHTABLE_TOTAL_CAPACITY,
            FUSE_MIN_HASHTABLE_TOTAL_CAPACITY,
            FUSE_MAX_HASHTABLE_TOTAL_CAPACITY);

    GROUPS_CACHE_ELEMENT_LIMIT = iniGetIntCorrectValue(ini_ctx,
            "element_limit", 64 * 1024, 16 * 1024, 1024 * 1024);
}

static void additional_groups_config_to_string(char *buff, const int size)
{
    if (!ADDITIONAL_GROUPS_ENABLED) {
        snprintf(buff, size, "groups_enabled: 0");
        return;
    }

    if (GROUPS_CACHE_ENABLED) {
        snprintf(buff, size, "groups_enabled: 1, "
                "groups-cache {enabled: 1, timeout: %d, "
                "shared_allocator_count: %d, "
                "hashtable_sharding_count: %d, "
                "hashtable_total_capacity: %d, "
                "element_limit: %d}",
                GROUPS_CACHE_TIMEOUT, GROUPS_CACHE_ALLOCATOR_COUNT,
                GROUPS_CACHE_SHARDING_COUNT, GROUPS_CACHE_HTABLE_CAPACITY,
                GROUPS_CACHE_ELEMENT_LIMIT);
    } else {
        snprintf(buff, size, "groups_enabled: 1, "
                "groups-cache {enabled: 0}");
    }
}

static const char *get_allow_others_caption(
        const FUSEAllowOthersMode allow_others)
{
    switch (allow_others) {
        case allow_all:
            return FUSE_ALLOW_ALL_STR;
        case allow_root:
            return FUSE_ALLOW_ROOT_STR;
        default:
            return "";
    }
}

int fcfs_fuse_global_init(const char *config_filename)
{
    const bool publish = true;
    int result;
    BufferInfo sf_idempotency_config;
    char buff[256];
    char rdma_busy_polling[128];
    char owner_config[2 * NAME_MAX + 64];
    char additional_groups_config[256];
    char max_threads_buff[64];
    IniContext iniContext;
    IniFullContext ini_ctx;

    if ((result=iniLoadFromFile(config_filename, &iniContext)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load conf file \"%s\" fail, ret code: %d",
                __LINE__, config_filename, result);
        return result;
    }

    FAST_INI_SET_FULL_CTX_EX(ini_ctx, config_filename,
            FCFS_API_DEFAULT_FASTDIR_SECTION_NAME, &iniContext);
    do {
        if ((result=load_fuse_config(&ini_ctx)) != 0) {
            break;
        }
        if (ADDITIONAL_GROUPS_ENABLED) {
            load_additional_groups_config(&ini_ctx);
        }

        if ((result=fcfs_api_pooled_init1_with_auth(
                        g_fuse_global_vars.nsmp.ns,
                        &ini_ctx, publish)) != 0)
        {
            break;
        }

        if ((result=fcfs_api_check_mountpoint1(config_filename,
                        g_fuse_global_vars.nsmp.mountpoint)) != 0)
        {
            break;
        }

        if ((result=fcfs_api_load_idempotency_config(
                        NULL, &ini_ctx)) != 0)
        {
            break;
        }
    } while (0);

    iniFreeContext(&iniContext);
    if (result != 0) {
        return result;
    }

    sf_idempotency_config.buff = buff;
    sf_idempotency_config.alloc_size = sizeof(buff);
    fcfs_api_log_client_common_configs(&g_fcfs_api_ctx,
            FCFS_API_DEFAULT_FASTDIR_SECTION_NAME,
            FCFS_API_DEFAULT_FASTSTORE_SECTION_NAME,
            &sf_idempotency_config, owner_config);

#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 12)
    sprintf(max_threads_buff, "max_idle_threads: %d",
            g_fuse_global_vars.max_idle_threads);
#else
    sprintf(max_threads_buff, "max_threads: %d, max_idle_threads: %d",
            g_fuse_global_vars.max_threads, g_fuse_global_vars.
            max_idle_threads);
#endif

    additional_groups_config_to_string(additional_groups_config,
            sizeof(additional_groups_config));

    if (g_fcfs_api_ctx.rdma.enabled) {
        sprintf(rdma_busy_polling, "rdma busy polling: %s, ",
                g_fcfs_api_ctx.rdma.busy_polling ? "true" : "false");
    } else {
        *rdma_busy_polling = '\0';
    }

    logInfo("FastCFS V%d.%d.%d, FUSE library version "
            "{compile: %d.%d, runtime: %s}, %s"
            "FastDIR namespace: %s, %sFUSE mountpoint: %s, "
            "%s, singlethread: %d, clone_fd: %d, "
            "%s, allow_others: %s, auto_unmount: %d, read_only: %d, "
            "attribute_timeout: %.1fs, entry_timeout: %.1fs, "
            "xattr_enabled: %d, writeback_cache: %d, kernel_cache: %d, %s",
            g_fcfs_global_vars.version.major,
            g_fcfs_global_vars.version.minor,
            g_fcfs_global_vars.version.patch,
            FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION,
            fuse_pkgversion(), rdma_busy_polling,
            g_fuse_global_vars.nsmp.ns,
            sf_idempotency_config.buff,
            g_fuse_global_vars.nsmp.mountpoint,
            owner_config, g_fuse_global_vars.singlethread,
            g_fuse_global_vars.clone_fd, max_threads_buff,
            get_allow_others_caption(g_fuse_global_vars.allow_others),
            g_fuse_global_vars.auto_unmount,
            g_fuse_global_vars.read_only,
            g_fuse_global_vars.attribute_timeout,
            g_fuse_global_vars.entry_timeout,
            g_fuse_global_vars.xattr_enabled,
            g_fuse_global_vars.writeback_cache,
            g_fuse_global_vars.kernel_cache,
            additional_groups_config);

    return 0;
}
