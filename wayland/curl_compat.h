/*
 * The handful of libcurl declarations this host uses, for building on a
 * system that has libcurl.so.4 but not its headers (the Makefile picks this
 * when pkg-config can't find libcurl). The values are part of libcurl's
 * stable ABI. With the headers installed, <curl/curl.h> is used instead.
 */
#ifndef MESH_CURL_COMPAT_H
#define MESH_CURL_COMPAT_H

typedef void CURL;
typedef int CURLcode;
struct curl_slist;

#define CURLE_OK 0
#define CURL_GLOBAL_DEFAULT 3L

#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_TIMEOUT 13
#define CURLOPT_USERAGENT 10018
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_HEADERFUNCTION 20079
#define CURLINFO_RESPONSE_CODE 0x200002

CURLcode curl_global_init(long flags);
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *curl, int option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, int info, ...);
void curl_easy_reset(CURL *curl);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string);
void curl_slist_free_all(struct curl_slist *list);

#endif
