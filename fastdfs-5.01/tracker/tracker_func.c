/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//tracker_func.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <grp.h>
#include <pwd.h>
#include "fdfs_define.h"
#include "logger.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "shared_func.h"
#include "ini_file_reader.h"
#include "connection_pool.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "tracker_global.h"
#include "tracker_func.h"
#include "tracker_mem.h"
#include "local_ip_func.h"
#include "fdfs_shared_func.h"

#ifdef WITH_HTTPD
#include "fdfs_http_shared.h"
#endif

/* 获取上传文件所选择group的方式，如果是指定特定组，获取组名 */
static int tracker_load_store_lookup(const char *filename, \
		IniContext *pItemContext)
{
	char *pGroupName;

	/* 设置选择上传文件的组的方式 */
	g_groups.store_lookup = iniGetIntValue(NULL, "store_lookup", \
			pItemContext, FDFS_STORE_LOOKUP_ROUND_ROBIN);

	/* 如果是循环选择或者是均衡选择，store_group为空，直接返回 */
	if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN)
	{
		g_groups.store_group[0] = '\0';
		return 0;
	}

	if (g_groups.store_lookup == FDFS_STORE_LOOKUP_LOAD_BALANCE)
	{
		g_groups.store_group[0] = '\0';
		return 0;
	}

	if (g_groups.store_lookup != FDFS_STORE_LOOKUP_SPEC_GROUP)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", the value of \"store_lookup\" is " \
			"invalid, value=%d!", \
			__LINE__, filename, g_groups.store_lookup);
		return EINVAL;
	}

	/* 如果是指定特定组，获取组名 */
	pGroupName = iniGetStrValue(NULL, "store_group", pItemContext);
	if (pGroupName == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\" must have item " \
			"\"store_group\"!", __LINE__, filename);
		return ENOENT;
	}
	if (pGroupName[0] == '\0')
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", " \
			"store_group is empty!", __LINE__, filename);
		return EINVAL;
	}

	snprintf(g_groups.store_group, sizeof(g_groups.store_group), \
			"%s", pGroupName);

	/* 检查组名是否符合要求，需为数字或英文字母 */
	if (fdfs_validate_group_name(g_groups.store_group) != 0) \
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", " \
			"the group name \"%s\" is invalid!", \
			__LINE__, filename, g_groups.store_group);
		return EINVAL;
	}

	return 0;
}

/* 获取并设置storage id相关信息 */
static int tracker_load_storage_id_info(const char *config_filename, \
		IniContext *pItemContext)
{
	char *pIdType;

	/* 设置是否使用server id作为storage server的标识 */
	g_use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				pItemContext, false);
	if (!g_use_storage_id)
	{
		return 0;
	}

	/* 设置文件名中id格式 */
	pIdType = iniGetStrValue(NULL, "id_type_in_filename", \
			pItemContext);
	if (pIdType != NULL && strcasecmp(pIdType, "id") == 0)
	{
		g_id_type_in_filename = FDFS_ID_TYPE_SERVER_ID;
	}
	else
	{
		g_id_type_in_filename = FDFS_ID_TYPE_IP_ADDRESS;
	}

	/* 从文件中获取组名，id及其对应的ip地址 */
	return fdfs_load_storage_ids_from_file(config_filename, pItemContext);
}

/* 加载tracker服务器配置文件，设置相应变量，返回绑定ip地址 */
int tracker_load_from_conf_file(const char *filename, \
		char *bind_addr, const int addr_size)
{
	char *pBasePath;				/* 数据和日志文件的根目录 */
	char *pBindAddr;				/* 绑定IP地址 */
	char *pRunByGroup;			/* 运行程序用户组 */
	char *pRunByUser;			/* 运行程序用户 */
	char *pThreadStackSize;		/* 线程栈大小 */
	char *pSlotMinSize;			/* trunk file分配的最小字节数 */
	char *pSlotMaxSize;			/* 只有文件大小小于等于这个参数值的文件才会合并存储 */
	char *pSpaceThreshold;		/* 提前创建trunk file时，需要达到的空闲trunk大小 */
	char *pTrunkFileSize;			/* 合并存储的trunk file大小 */
	char *pRotateErrorLogSize;	/* 定期轮转error log的大小 */
#ifdef WITH_HTTPD
	char *pHttpCheckUri;
	char *pHttpCheckType;
#endif
	IniContext iniContext;
	int result;
	/* 配置文件值对应的整型变量 */
	int64_t thread_stack_size;
	int64_t trunk_file_size;
	int64_t slot_min_size;
	int64_t slot_max_size;
	int64_t rotate_error_log_size;
	char reserved_space_str[32];

	/* 初始化所有group信息 */
	memset(&g_groups, 0, sizeof(FDFSGroups));
	memset(&iniContext, 0, sizeof(IniContext));
	/* 加载配置文件到相应结构体对象中 */
	if ((result=iniLoadFromFile(filename, &iniContext)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"load conf file \"%s\" fail, ret code: %d", \
			__LINE__, filename, result);
		return result;
	}

	//iniPrintItems(&iniContext);

	/* 以下部分都是读取tracker配置文件信息到内存相应变量中 */
	do
	{
		if (iniGetBoolValue(NULL, "disabled", &iniContext, false))
		{
			logError("file: "__FILE__", line: %d, " \
				"conf file \"%s\" disabled=true, exit", \
				__LINE__, filename);
			result = ECANCELED;
			break;
		}

		/* 获取base_path */
		pBasePath = iniGetStrValue(NULL, "base_path", &iniContext);
		if (pBasePath == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"conf file \"%s\" must have item " \
				"\"base_path\"!", __LINE__, filename);
			result = ENOENT;
			break;
		}

		snprintf(g_fdfs_base_path, sizeof(g_fdfs_base_path), "%s", pBasePath);
		chopPath(g_fdfs_base_path);
		if (!fileExists(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" can't be accessed, error info: %s", \
				__LINE__, g_fdfs_base_path, STRERROR(errno));
			result = errno != 0 ? errno : ENOENT;
			break;
		}
		if (!isDir(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" is not a directory!", \
				__LINE__, g_fdfs_base_path);
			result = ENOTDIR;
			break;
		}

		/* 获取并设置日志级别 */
		load_log_level(&iniContext);

		/* 初始化日志及文件，将文件名及文件大小保存在全局变量中 */
		if ((result=log_set_prefix(g_fdfs_base_path, \
				TRACKER_ERROR_LOG_FILENAME)) != 0)
		{
			break;
		}

		/* 获取连接超时时间 */
		g_fdfs_connect_timeout = iniGetIntValue(NULL, "connect_timeout", \
				&iniContext, DEFAULT_CONNECT_TIMEOUT);
		if (g_fdfs_connect_timeout <= 0)
		{
			g_fdfs_connect_timeout = DEFAULT_CONNECT_TIMEOUT;
		}

		/* 获取网络超时时间 */
		g_fdfs_network_timeout = iniGetIntValue(NULL, "network_timeout", \
				&iniContext, DEFAULT_NETWORK_TIMEOUT);
		if (g_fdfs_network_timeout <= 0)
		{
			g_fdfs_network_timeout = DEFAULT_NETWORK_TIMEOUT;
		}
		g_network_tv.tv_sec = g_fdfs_network_timeout;

		/* 获取指定的tracker server端口号 */
		g_server_port = iniGetIntValue(NULL, "port", &iniContext, \
				FDFS_TRACKER_SERVER_DEF_PORT);
		if (g_server_port <= 0)
		{
			g_server_port = FDFS_TRACKER_SERVER_DEF_PORT;
		}

		/* 获取绑定ip地址 */
		pBindAddr = iniGetStrValue(NULL, "bind_addr", &iniContext);
		if (pBindAddr == NULL)
		{
			bind_addr[0] = '\0';
		}
		else
		{
			snprintf(bind_addr, addr_size, "%s", pBindAddr);
		}

		/* 获取上传文件所选择group的方式，如果是指定特定组，获取组名 */
		if ((result=tracker_load_store_lookup(filename, \
			&iniContext)) != 0)
		{
			break;
		}

		/* 获取上传文件时同一个组中选择哪一个storage的方式 */
		g_groups.store_server = (byte)iniGetIntValue(NULL, \
				"store_server",  &iniContext, \
				FDFS_STORE_SERVER_ROUND_ROBIN);
		/* 如果不存在所设置的方式，默认为循环选择方式 */
		if (!(g_groups.store_server == FDFS_STORE_SERVER_FIRST_BY_IP ||\
			g_groups.store_server == FDFS_STORE_SERVER_FIRST_BY_PRI||
			g_groups.store_server == FDFS_STORE_SERVER_ROUND_ROBIN))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"store_server 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.store_server, \
				FDFS_STORE_SERVER_ROUND_ROBIN);

			g_groups.store_server = FDFS_STORE_SERVER_ROUND_ROBIN;
		}

		/* 获取下载文件时选择哪一个storage的方式 */
		g_groups.download_server = (byte)iniGetIntValue(NULL, \
			"download_server", &iniContext, \
			FDFS_DOWNLOAD_SERVER_ROUND_ROBIN);
		if (!(g_groups.download_server==FDFS_DOWNLOAD_SERVER_ROUND_ROBIN
			|| g_groups.download_server == 
				FDFS_DOWNLOAD_SERVER_SOURCE_FIRST))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"download_server 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.download_server, \
				FDFS_DOWNLOAD_SERVER_ROUND_ROBIN);

			g_groups.download_server = \
				FDFS_DOWNLOAD_SERVER_ROUND_ROBIN;
		}

		/* 获取上传文件时选择哪一个目录的方式 */
		g_groups.store_path = (byte)iniGetIntValue(NULL, "store_path", \
			&iniContext, FDFS_STORE_PATH_ROUND_ROBIN);
		if (!(g_groups.store_path == FDFS_STORE_PATH_ROUND_ROBIN || \
			g_groups.store_path == FDFS_STORE_PATH_LOAD_BALANCE))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"store_path 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.store_path , \
				FDFS_STORE_PATH_ROUND_ROBIN);
			g_groups.store_path = FDFS_STORE_PATH_ROUND_ROBIN;
		}

		/* 获取并设置每台storage server保留存储空间大小 */
		if ((result=fdfs_parse_storage_reserved_space(&iniContext, \
				&g_storage_reserved_space)) != 0)
		{
			break;
		}

		/* 获取最大连接数 */
		g_max_connections = iniGetIntValue(NULL, "max_connections", \
				&iniContext, DEFAULT_MAX_CONNECTONS);
		if (g_max_connections <= 0)
		{
			g_max_connections = DEFAULT_MAX_CONNECTONS;
		}

		/* 获取accept 线程数 */
		g_accept_threads = iniGetIntValue(NULL, "accept_threads", \
				&iniContext, 1);
		if (g_accept_threads <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"accept_threads\" is invalid, " \
				"value: %d <= 0!", __LINE__, g_accept_threads);
			result = EINVAL;
                        break;
		}

		/* 获取工作线程数 */
		g_work_threads = iniGetIntValue(NULL, "work_threads", \
				&iniContext, DEFAULT_WORK_THREADS);
		if (g_work_threads <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"work_threads\" is invalid, " \
				"value: %d <= 0!", __LINE__, g_work_threads);
			result = EINVAL;
                        break;
		}

		/* 设置每个进程最多能打开的文件数 */
		if ((result=set_rlimit(RLIMIT_NOFILE, g_max_connections)) != 0)
		{
			break;
		}

		/* 获取运行用户组及用户，如果不指定，就为当前用户组及用户 */
		pRunByGroup = iniGetStrValue(NULL, "run_by_group", &iniContext);
		pRunByUser = iniGetStrValue(NULL, "run_by_user", &iniContext);
		if (pRunByGroup == NULL)
		{
			*g_run_by_group = '\0';
		}
		else
		{
			snprintf(g_run_by_group, sizeof(g_run_by_group), \
				"%s", pRunByGroup);
		}
		if (*g_run_by_group == '\0')
		{
			g_run_by_gid = getegid();
		}
		else
		{
			struct group *pGroup;

			/* 根据用户组名获取gid */
     			pGroup = getgrnam(g_run_by_group);
			if (pGroup == NULL)
			{
				result = errno != 0 ? errno : ENOENT;
				logError("file: "__FILE__", line: %d, " \
					"getgrnam fail, errno: %d, " \
					"error info: %s", __LINE__, \
					result, STRERROR(result));
				return result;
			}

			g_run_by_gid = pGroup->gr_gid;
		}


		if (pRunByUser == NULL)
		{
			*g_run_by_user = '\0';
		}
		else
		{
			snprintf(g_run_by_user, sizeof(g_run_by_user), \
				"%s", pRunByUser);
		}
		if (*g_run_by_user == '\0')
		{
			g_run_by_uid = geteuid();
		}
		else
		{
			struct passwd *pUser;

			/* 根据用户名获取uid */
     			pUser = getpwnam(g_run_by_user);
			if (pUser == NULL)
			{
				result = errno != 0 ? errno : ENOENT;
				logError("file: "__FILE__", line: %d, " \
					"getpwnam fail, errno: %d, " \
					"error info: %s", __LINE__, \
					result, STRERROR(result));
				return result;
			}

			g_run_by_uid = pUser->pw_uid;
		}

		/* 获取并设置允许连接ip地址 */
		if ((result=load_allow_hosts(&iniContext, \
                	 &g_allow_ip_addrs, &g_allow_ip_count)) != 0)
		{
			return result;
		}

		/* 获取并设置日志更新到文件中的间隔时间 */
		g_sync_log_buff_interval = iniGetIntValue(NULL, \
				"sync_log_buff_interval", &iniContext, \
				SYNC_LOG_BUFF_DEF_INTERVAL);
		if (g_sync_log_buff_interval <= 0)
		{
			g_sync_log_buff_interval = SYNC_LOG_BUFF_DEF_INTERVAL;
		}

		/* 获取并设置检查storage状态间隔时间 */
		g_check_active_interval = iniGetIntValue(NULL, \
				"check_active_interval", &iniContext, \
				CHECK_ACTIVE_DEF_INTERVAL);
		if (g_check_active_interval <= 0)
		{
			g_check_active_interval = CHECK_ACTIVE_DEF_INTERVAL;
		}

		/* 获取并设置线程栈的大小 */
		pThreadStackSize = iniGetStrValue(NULL, \
			"thread_stack_size", &iniContext);
		if (pThreadStackSize == NULL)
		{
			/* 线程栈默认为64KB */
			thread_stack_size = 64 * 1024;
		}
		else if ((result=parse_bytes(pThreadStackSize, 1, \
				&thread_stack_size)) != 0)
		{
			return result;
		}
		g_thread_stack_size = (int)thread_stack_size;
		printf("wcl: thread_stack_size: %d\n", g_thread_stack_size);

		/*
		 * 该参数控制当storage server ip改变时集群是否自动调整
		 * 只有在storage server端进程重启时才完成调整
		 */
		g_storage_ip_changed_auto_adjust = iniGetBoolValue(NULL, \
				"storage_ip_changed_auto_adjust", \
				&iniContext, true);

		/* 设置storage server之间同步文件的最大延迟时间 */
		g_storage_sync_file_max_delay = iniGetIntValue(NULL, \
				"storage_sync_file_max_delay", &iniContext, \
				DEFAULT_STORAGE_SYNC_FILE_MAX_DELAY);
		if (g_storage_sync_file_max_delay <= 0)
		{
			g_storage_sync_file_max_delay = \
					DEFAULT_STORAGE_SYNC_FILE_MAX_DELAY;
		}

		/* 设置storage server之间同步一个文件的最大超时时间 */
		g_storage_sync_file_max_time = iniGetIntValue(NULL, \
				"storage_sync_file_max_time", &iniContext, \
				DEFAULT_STORAGE_SYNC_FILE_MAX_TIME);
		if (g_storage_sync_file_max_time <= 0)
		{
			g_storage_sync_file_max_time = \
				DEFAULT_STORAGE_SYNC_FILE_MAX_TIME;
		}

		/* 设置是否使用小文件合并存储特性 */
		g_if_use_trunk_file = iniGetBoolValue(NULL, \
			"use_trunk_file", &iniContext, false);

		/* 设置trunk file分配的最小字节数 */
		pSlotMinSize = iniGetStrValue(NULL, \
			"slot_min_size", &iniContext);
		if (pSlotMinSize == NULL)
		{
			slot_min_size = 256;
		}
		else if ((result=parse_bytes(pSlotMinSize, 1, \
				&slot_min_size)) != 0)
		{
			return result;
		}
		g_slot_min_size = (int)slot_min_size;
		if (g_slot_min_size <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"slot_min_size\" %d is invalid, " \
				"which <= 0", __LINE__, g_slot_min_size);
			result = EINVAL;
			break;
		}
		if (g_slot_min_size > 64 * 1024)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"slot_min_size\" %d is too large, " \
				"change to 64KB", __LINE__, g_slot_min_size);
			g_slot_min_size = 64 * 1024;
		}

		/* 设置合并存储的trunk file大小 */
		pTrunkFileSize = iniGetStrValue(NULL, \
			"trunk_file_size", &iniContext);
		if (pTrunkFileSize == NULL)
		{
			trunk_file_size = 64 * 1024 * 1024;
		}
		else if ((result=parse_bytes(pTrunkFileSize, 1, \
				&trunk_file_size)) != 0)
		{
			return result;
		}
		g_trunk_file_size = (int)trunk_file_size;
		if (g_trunk_file_size < 4 * 1024 * 1024)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"trunk_file_size\" %d is too small, " \
				"change to 4MB", __LINE__, g_trunk_file_size);
			g_trunk_file_size = 4 * 1024 * 1024;
		}

		/* 只有文件大小<= 这个参数的文件才会合并 */
		pSlotMaxSize = iniGetStrValue(NULL, \
			"slot_max_size", &iniContext);
		if (pSlotMaxSize == NULL)
		{
			/* 如果没有设置，默认为trunk file大小的一半 */
			slot_max_size = g_trunk_file_size / 2;
		}
		else if ((result=parse_bytes(pSlotMaxSize, 1, \
				&slot_max_size)) != 0)
		{
			return result;
		}
		g_slot_max_size = (int)slot_max_size;
		if (g_slot_max_size <= g_slot_min_size)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"slot_max_size\" %d is invalid, " \
				"which <= slot_min_size: %d", \
				__LINE__, g_slot_max_size, g_slot_min_size);
			result = EINVAL;
			break;
		}
		if (g_slot_max_size > g_trunk_file_size / 2)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"slot_max_size\": %d is too large, " \
				"change to %d", __LINE__, g_slot_max_size, \
				g_trunk_file_size / 2);
			g_slot_max_size = g_trunk_file_size / 2;
		}

		/* 设置是否提前创建trunk file */
		g_trunk_create_file_advance = iniGetBoolValue(NULL, \
			"trunk_create_file_advance", &iniContext, false);

		/* 设置提前创建trunk file的起始时间点 */
		if ((result=get_time_item_from_conf(&iniContext, \
                	"trunk_create_file_time_base", \
			&g_trunk_create_file_time_base, 2, 0)) != 0)
		{
			return result;
		}

		/* 设置创建trunk file的时间间隔 */
		g_trunk_create_file_interval = iniGetIntValue(NULL, \
				"trunk_create_file_interval", &iniContext, \
				86400);

		/* 设置如果提前创建trunk file时，需要达到的空闲trunk大小 */
		pSpaceThreshold = iniGetStrValue(NULL, \
			"trunk_create_file_space_threshold", &iniContext);
		if (pSpaceThreshold == NULL)
		{
			g_trunk_create_file_space_threshold = 0;
		}
		else if ((result=parse_bytes(pSpaceThreshold, 1, \
				&g_trunk_create_file_space_threshold)) != 0)
		{
			return result;
		}

		/* 设置trunk file binlog的压缩时间间隔 */
		g_trunk_compress_binlog_min_interval = iniGetIntValue(NULL, \
				"trunk_compress_binlog_min_interval", \
				&iniContext, 0);

		/* 设置trunk初始化时，是否检查可用空间被占用 */
		g_trunk_init_check_occupying = iniGetBoolValue(NULL, \
			"trunk_init_check_occupying", &iniContext, false);

		/* 设置是否无条件从trunk binlog中加载trunk可用空间信息 */
		g_trunk_init_reload_from_binlog = iniGetBoolValue(NULL, \
			"trunk_init_reload_from_binlog", &iniContext, false);

		/* 获取并设置storage id相关信息 */
		if ((result=tracker_load_storage_id_info( \
				filename, &iniContext)) != 0)
		{
			return result;
		}

		/* 设置error log日志是否每天轮转 */
		g_rotate_error_log = iniGetBoolValue(NULL, "rotate_error_log",\
					&iniContext, false);

		/* 设置error log每天轮转的时间点 */
		if ((result=get_time_item_from_conf(&iniContext, \
			"error_log_rotate_time", &g_error_log_rotate_time, \
			0, 0)) != 0)
		{
			break;
		}

		/* 设置error log达到多大后进行轮转 */
		pRotateErrorLogSize = iniGetStrValue(NULL, \
			"rotate_error_log_size", &iniContext);
		if (pRotateErrorLogSize == NULL)
		{
			rotate_error_log_size = 0;
		}
		else if ((result=parse_bytes(pRotateErrorLogSize, 1, \
				&rotate_error_log_size)) != 0)
		{
			break;
		}
		if (rotate_error_log_size > 0 && \
			rotate_error_log_size < FDFS_ONE_MB)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"rotate_error_log_size\": " \
				INT64_PRINTF_FORMAT" is too small, " \
				"change to 1 MB", __LINE__, \
				rotate_error_log_size);
			rotate_error_log_size = FDFS_ONE_MB;
		}
		g_log_context.rotate_size = rotate_error_log_size;

		/* 设置重复文件是否使用链接存储 */
		g_store_slave_file_use_link = iniGetBoolValue(NULL, \
			"store_slave_file_use_link", &iniContext, false);

		/* 连接池相关的初始化工作 */
		if ((result=fdfs_connection_pool_init(filename, &iniContext)) != 0)
		{
			break;
		}

#ifdef WITH_HTTPD
		if ((result=fdfs_http_params_load(&iniContext, \
				filename, &g_http_params)) != 0)
		{
			return result;
		}

		g_http_check_interval = iniGetIntValue(NULL, \
			"http.check_alive_interval", &iniContext, 30);

		pHttpCheckType = iniGetStrValue(NULL, \
			"http.check_alive_type", &iniContext);
		if (pHttpCheckType != NULL && \
			strcasecmp(pHttpCheckType, "http") == 0)
		{
			g_http_check_type = FDFS_HTTP_CHECK_ALIVE_TYPE_HTTP;
		}
		else
		{
			g_http_check_type = FDFS_HTTP_CHECK_ALIVE_TYPE_TCP;
		}

		pHttpCheckUri = iniGetStrValue(NULL, \
			"http.check_alive_uri", &iniContext);
		if (pHttpCheckUri == NULL)
		{
			*g_http_check_uri = '/';
			*(g_http_check_uri+1) = '\0';
		}
		else if (*pHttpCheckUri == '/')
		{
			snprintf(g_http_check_uri, sizeof(g_http_check_uri), \
				"%s", pHttpCheckUri);
		}
		else
		{
			snprintf(g_http_check_uri, sizeof(g_http_check_uri), \
				"/%s", pHttpCheckUri);
		}


#endif

		logInfo("FastDFS v%d.%02d, base_path=%s, " \
			"run_by_group=%s, run_by_user=%s, " \
			"connect_timeout=%ds, "    \
			"network_timeout=%ds, "    \
			"port=%d, bind_addr=%s, " \
			"max_connections=%d, "    \
			"accept_threads=%d, "    \
			"work_threads=%d, "    \
			"store_lookup=%d, store_group=%s, " \
			"store_server=%d, store_path=%d, " \
			"reserved_storage_space=%s, " \
			"download_server=%d, " \
			"allow_ip_count=%d, sync_log_buff_interval=%ds, " \
			"check_active_interval=%ds, " \
			"thread_stack_size=%d KB, " \
			"storage_ip_changed_auto_adjust=%d, "  \
			"storage_sync_file_max_delay=%ds, " \
			"storage_sync_file_max_time=%ds, "  \
			"use_trunk_file=%d, " \
			"slot_min_size=%d, " \
			"slot_max_size=%d MB, " \
			"trunk_file_size=%d MB, " \
			"trunk_create_file_advance=%d, " \
			"trunk_create_file_time_base=%02d:%02d, " \
			"trunk_create_file_interval=%d, " \
			"trunk_create_file_space_threshold=%d GB, " \
			"trunk_init_check_occupying=%d, " \
			"trunk_init_reload_from_binlog=%d, " \
			"trunk_compress_binlog_min_interval=%d, " \
			"use_storage_id=%d, " \
			"id_type_in_filename=%s, " \
			"storage_id_count=%d, " \
			"rotate_error_log=%d, " \
			"error_log_rotate_time=%02d:%02d, " \
			"rotate_error_log_size="INT64_PRINTF_FORMAT", " \
			"store_slave_file_use_link=%d, " \
			"use_connection_pool=%d, " \
			"g_connection_pool_max_idle_time=%ds", \
			g_fdfs_version.major, g_fdfs_version.minor,  \
			g_fdfs_base_path, g_run_by_group, g_run_by_user, \
			g_fdfs_connect_timeout, \
			g_fdfs_network_timeout, g_server_port, bind_addr, \
			g_max_connections, g_accept_threads, g_work_threads, \
			g_groups.store_lookup, g_groups.store_group, \
			g_groups.store_server, g_groups.store_path, \
			fdfs_storage_reserved_space_to_string( \
			    &g_storage_reserved_space, reserved_space_str), \
			g_groups.download_server, \
			g_allow_ip_count, g_sync_log_buff_interval, \
			g_check_active_interval, g_thread_stack_size / 1024, \
			g_storage_ip_changed_auto_adjust, \
			g_storage_sync_file_max_delay, \
			g_storage_sync_file_max_time, \
			g_if_use_trunk_file, g_slot_min_size, \
			g_slot_max_size / FDFS_ONE_MB, \
			g_trunk_file_size / FDFS_ONE_MB, \
			g_trunk_create_file_advance, \
			g_trunk_create_file_time_base.hour, \
			g_trunk_create_file_time_base.minute, \
			g_trunk_create_file_interval, \
			(int)(g_trunk_create_file_space_threshold / \
			(FDFS_ONE_MB * 1024)), g_trunk_init_check_occupying, \
			g_trunk_init_reload_from_binlog, \
			g_trunk_compress_binlog_min_interval, \
			g_use_storage_id, g_id_type_in_filename == \
			FDFS_ID_TYPE_SERVER_ID ? "id" : "ip", g_storage_id_count, \
			g_rotate_error_log, g_error_log_rotate_time.hour, \
			g_error_log_rotate_time.minute, \
			g_log_context.rotate_size, g_store_slave_file_use_link, \
			g_use_connection_pool, g_connection_pool_max_idle_time);

#ifdef WITH_HTTPD
		if (!g_http_params.disabled)
		{
			logInfo("HTTP supported: " \
				"server_port=%d, " \
				"default_content_type=%s, " \
				"anti_steal_token=%d, " \
				"token_ttl=%ds, " \
				"anti_steal_secret_key length=%d, "  \
				"token_check_fail content_type=%s, " \
				"token_check_fail buff length=%d, "  \
				"check_active_interval=%d, " \
				"check_active_type=%s, " \
				"check_active_uri=%s",  \
				g_http_params.server_port, \
				g_http_params.default_content_type, \
				g_http_params.anti_steal_token, \
				g_http_params.token_ttl, \
				g_http_params.anti_steal_secret_key.length, \
				g_http_params.token_check_fail_content_type, \
				g_http_params.token_check_fail_buff.length, \
				g_http_check_interval, g_http_check_type == \
				FDFS_HTTP_CHECK_ALIVE_TYPE_TCP ? "tcp":"http",\
				g_http_check_uri);
		}
#endif

	} while (0);

	/* 释放存放配置文件内容的对象空间 */
	iniFreeContext(&iniContext);

	/* 初始化所有本地ip地址信息 */
	load_local_host_ip_addrs();

	return result;
}

