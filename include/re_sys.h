/**
 * @file re_sys.h  Interface to system module
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdio.h>

#ifndef RE_VERSION
#define RE_VERSION "?"
#endif

/**
 * @def ARCH
 *
 * Architecture
 */
#ifndef ARCH
#define ARCH "?"
#endif

/**
 * @def OS
 *
 * Operating System
 */
#ifndef OS
#ifdef WIN32
#define OS "win32"
#else
#define OS "?"
#endif
#endif

struct re_printf;
struct mbuf;

int  sys_kernel_get(struct re_printf *pf, void *unused);
int  sys_build_get(struct re_printf *pf, void *unused);
const char *sys_arch_get(void);
const char *sys_os_get(void);
const char *sys_libre_version_get(void);
const char *sys_username(void);
int sys_getenv(char **env, const char *name);
int sys_coredump_set(bool enable);
int sys_daemon(void);
void sys_usleep(unsigned int us);

static inline void sys_msleep(unsigned int ms)
{
	sys_usleep(ms * 1000);
}


uint16_t sys_htols(uint16_t v);
uint32_t sys_htoll(uint32_t v);
uint16_t sys_ltohs(uint16_t v);
uint32_t sys_ltohl(uint32_t v);
uint64_t sys_htonll(uint64_t v);
uint64_t sys_ntohll(uint64_t v);


/* Random */
uint16_t rand_u16(void);
uint32_t rand_u32(void);
uint64_t rand_u64(void);
char     rand_char(void);
void     rand_str(char *str, size_t size);
void     rand_bytes(uint8_t *p, size_t size);
void     rand_close(void);


/* File-System */
int  re_fs_mkdir(const char *path, uint16_t mode);
int  re_fs_gethome(char *path, size_t sz);
bool re_fs_isdir(const char *path);
bool re_fs_isfile(const char *file);
int  re_fs_open(int *fdp, const char *file, int flags);
int  re_fs_fopen(FILE **fp, const char *file, const char *mode);
int  re_fs_fread(struct mbuf **mbp, const char *path);

void re_fs_stdio_hide(void);
void re_fs_stdio_restore(void);
