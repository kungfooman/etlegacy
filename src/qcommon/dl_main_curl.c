/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * ET: Legacy
 * Copyright (C) 2012-2017 ET:Legacy team <mail@etlegacy.com>
 *
 * This file is part of ET: Legacy - http://www.etlegacy.com
 *
 * ET: Legacy is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ET: Legacy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ET: Legacy. If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, Wolfenstein: Enemy Territory GPL Source Code is also
 * subject to certain additional terms. You should have received a copy
 * of these additional terms immediately following the terms and conditions
 * of the GNU General Public License which accompanied the source code.
 * If not, please request a copy in writing from id Software at the address below.
 *
 * id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
 */
/**
 * @file dl_main_curl.c
 *
 * @todo Additional features that would be nice for this code:
 *           Only display \<gamepath\>/\<file\>, e.g., etpro/etpro-3_0_1.pk3 in the UI.
 *           Add server as referring URL
 */

#include <curl/curl.h>

#include "q_shared.h"
#include "qcommon.h"
#include "dl_public.h"

#define APP_NAME        "ID_DOWNLOAD"
#define APP_VERSION     "2.0"

#define GET_BUFFER_SIZE 1024 * 256

/**
 * @var Initialize once
 */
static int dl_initialized = 0;

static CURLM *dl_multi   = NULL;
static CURL  *dl_request = NULL;
static FILE  *dl_file    = NULL;

/**
 * @struct write_result_t
 */
typedef struct write_result_s
{
	char *data;
	int pos;
} write_result_t;

/**
 * @brief DL_cb_FWriteFile
 * @param[in] ptr
 * @param[in] size
 * @param[in] nmemb
 * @param[in] stream
 * @return
 */
static size_t DL_cb_FWriteFile(void *ptr, size_t size, size_t nmemb, void *stream)
{
	FILE *file = (FILE *)stream;
	return fwrite(ptr, size, nmemb, file);
}

/**
 * @brief DL_cb_Progress
 * @param clientp - unused
 * @param dltotal - unused
 * @param[in] dlnow
 * @param ultotal - unused
 * @param ulnow   - unused
 * @return
 *
 * @note cl_downloadSize and cl_downloadTime are set by the Q3 protocol...
 * and it would probably be expensive to verify them here.
 */
static int DL_cb_Progress(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	Cvar_SetValue("cl_downloadCount", (float)dlnow);
	return 0;
}

/**
 * @brief DL_write_function
 * @param[in] ptr
 * @param[in] size
 * @param[in] nmemb
 * @param[out] stream
 * @return
 */
size_t DL_write_function(void *ptr, size_t size, size_t nmemb, void *stream)
{
	write_result_t *result = (write_result_t *)stream;

	if (result->pos + size * nmemb >= GET_BUFFER_SIZE - 1)
	{
		Com_Printf(S_COLOR_RED  "DL_write_function: Error - buffer is too small");
		return 0;
	}

	memcpy(result->data + result->pos, ptr, size * nmemb);
	result->pos += size * nmemb;

	return size * nmemb;
}

/**
 * @brief DL_InitDownload
 */
void DL_InitDownload(void)
{
	if (dl_initialized)
	{
		return;
	}

	/* Make sure curl has initialized, so the cleanup doesn't get confused */
	curl_global_init(CURL_GLOBAL_ALL);

	dl_multi = curl_multi_init();

	Com_Printf("Client download subsystem initialized\n");
	dl_initialized = 1;
}

/**
 * @brief DL_Shutdown
 */
void DL_Shutdown(void)
{
	if (!dl_initialized)
	{
		return;
	}

	curl_multi_cleanup(dl_multi);
	dl_multi = NULL;

	curl_global_cleanup();

	dl_initialized = 0;
}

/**
 * @brief Inspired from http://www.w3.org/Library/Examples/LoadToFile.c
 * setup the download, return once we have a connection
 *
 * @param localName
 * @param remoteName
 * @return
 */
int DL_BeginDownload(char *localName, const char *remoteName)
{
	char referer[MAX_STRING_CHARS + 5 /*"ET://"*/];

	if (dl_request)
	{
		Com_Printf(S_COLOR_RED "DL_BeginDownload: Error - called with a download request already active\n");
		return 0;
	}

	if (!localName[0] || !remoteName[0])
	{
		Com_Printf(S_COLOR_RED "DL_BeginDownload: Error - empty download URL or empty local file name\n");
		return 0;
	}

	if (FS_CreatePath(localName))
	{
		Com_Printf(S_COLOR_RED "DL_BeginDownload: Error - unable to create directory (%s).\n", localName);
		return 0;
	}

	dl_file = fopen(localName, "wb+");
	if (!dl_file)
	{
		Com_Printf(S_COLOR_RED  "DL_BeginDownload: Error - unable to open '%s' for writing\n", localName);
		return 0;
	}

	DL_InitDownload();

	/* ET://ip:port */
	strcpy(referer, "ET://");
	Q_strncpyz(referer + 5, Cvar_VariableString("cl_currentServerIP"), MAX_STRING_CHARS);

	dl_request = curl_easy_init();
	curl_easy_setopt(dl_request, CURLOPT_USERAGENT, va("%s %s", APP_NAME "/" APP_VERSION, curl_version()));
	curl_easy_setopt(dl_request, CURLOPT_REFERER, referer);
	curl_easy_setopt(dl_request, CURLOPT_URL, remoteName);
	curl_easy_setopt(dl_request, CURLOPT_WRITEFUNCTION, DL_cb_FWriteFile);
	curl_easy_setopt(dl_request, CURLOPT_WRITEDATA, (void *)dl_file);
	curl_easy_setopt(dl_request, CURLOPT_PROGRESSFUNCTION, DL_cb_Progress);
	curl_easy_setopt(dl_request, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(dl_request, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(dl_request, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(dl_request, CURLOPT_MAXREDIRS, 5);

	curl_multi_add_handle(dl_multi, dl_request);

	Cvar_Set("cl_downloadName", remoteName);

	return 1;
}

/**
 * @brief DL_GetString
 * @param[in] url
 * @return
 */
char *DL_GetString(const char *url)
{
	CURL     *curl = NULL;
	CURLcode status;
	char     *data = NULL;
	long     code;

	write_result_t write_result = { NULL, 0 };

	if (!url)
	{
		Com_Printf(S_COLOR_RED "DL_GetString: Error - empty download URL\n");
		return NULL;
	}

	DL_InitDownload();

	curl = curl_easy_init();

	data = (char *)malloc(GET_BUFFER_SIZE);
	if (!data)
	{
		goto error_get;
	}
	else
	{
		write_result.data = data;
	}

	curl_easy_setopt(curl, CURLOPT_USERAGENT, va("%s %s", APP_NAME "/" APP_VERSION, curl_version()));
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DL_write_function);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&write_result);

	status = curl_easy_perform(curl);
	if (status != 0)
	{
		Com_Printf(S_COLOR_RED "DL_GetString: Error - unable to request data from %s\n", url);
		Com_Printf(S_COLOR_RED "DL_GetString: Error - %s\n", curl_easy_strerror(status));
		goto error_get;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	if (code != 200)
	{
		Com_Printf(S_COLOR_RED "DL_GetString: Error - server responded with code %ld\n", code);
		goto error_get;
	}

	curl_easy_cleanup(curl);
	data[write_result.pos] = '\0';

	return data;

error_get:

	if (curl)
	{
		curl_easy_cleanup(curl);
	}

	if (data)
	{
		free(data);
	}

	return NULL;
}

/**
 * @brief DL_DownloadLoop
 * @return
 *
 * @note maybe this should be CL_DL_DownloadLoop
 */
dlStatus_t DL_DownloadLoop(void)
{
	CURLMcode  status;
	CURLMsg    *msg;
	int        dls  = 0;
	const char *err = NULL;

	if (!dl_request)
	{
		Com_DPrintf("DL_DownloadLoop: Error - unexpected call with dl_request == NULL\n");
		return DL_DONE;
	}

	if ((status = curl_multi_perform(dl_multi, &dls)) == CURLM_CALL_MULTI_PERFORM && dls)
	{
		return DL_CONTINUE;
	}

	while ((msg = curl_multi_info_read(dl_multi, &dls)) && msg->easy_handle != dl_request)
		;

	if (!msg || msg->msg != CURLMSG_DONE)
	{
		return DL_CONTINUE;
	}

	if (msg->data.result != CURLE_OK)
	{
		err = curl_easy_strerror(msg->data.result);
	}
	else
	{
		err = NULL;
	}

	curl_multi_remove_handle(dl_multi, dl_request);
	curl_easy_cleanup(dl_request);

	fclose(dl_file);
	dl_file = NULL;

	dl_request = NULL;

	Cvar_Set("ui_dl_running", "0");

	if (err)
	{
		Com_Printf(S_COLOR_RED "DL_DownloadLoop: Error - request terminated with failure status '%s'\n", err);
		return DL_FAILED;
	}

	return DL_DONE;
}
