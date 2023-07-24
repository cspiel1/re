/**
 * @file nofs.c  File-system stub functions
 *
 * Copyright (C) 2023 - Christian Spielberger
 */
#include <errno.h>
#include <stdint.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_sys.h>
#include <re_mem.h>
#include <re_mbuf.h>


#define DEBUG_MODULE "nofs"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

int re_fs_mkdir(const char *path, uint16_t mode)
{
	(void)path;
	(void)mode;
	return ENOSYS;
}


/**
 * Get the home directory for the current user
 *
 * @param path String to write home directory
 * @param sz   Size of path string
 *
 * @return 0 if success, otherwise errorcode
 */
int re_fs_gethome(char *path, size_t sz)
{
	(void)path;
	(void)sz;
	return ENOSYS;
}


bool re_fs_isdir(const char *path)
{
	(void)path;
	return false;
}


bool re_fs_isfile(const char *file)
{
	(void)file;
	return false;
}


int  re_fs_open(int *fdp, const char *file, int flags)
{
	(void)fdp;
	(void)file;
	(void)flags;

	return ENOSYS;
}


int re_fs_fopen(FILE **fp, const char *file, const char *mode)
{
	(void)fp;
	(void)file;
	(void)mode;

	return ENOSYS;
}


void re_fs_stdio_hide(void)
{
	return;
}


/**
 * Restore stdout and stderr output (no THREAD-SAFETY)
 */
void re_fs_stdio_restore(void)
{
	return;
}


int re_fs_fread(struct mbuf **mbp, const char *path)
{
	(void)mbp;
	(void)path;

	return ENOSYS;
}
