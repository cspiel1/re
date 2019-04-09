/**
 * @file basic.c HTTP Basic authentication
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re_types.h>
#include <re_mbuf.h>
#include <re_base64.h>
#include <re_mem.h>
#include <re_fmt.h>
#include <re_httpauth.h>


/**
 * Decode a Basic response
 *
 * @param basic Basic response object
 * @param hval Header value to decode from
 *
 * @return 0 if successfully decoded, otherwise errorcode
 */
int httpauth_basic_decode(struct httpauth_basic *basic,
				    const struct pl *hval)
{
	if (!basic || !hval)
		return EINVAL;

	if (re_regex(hval->p, hval->l, "[ \t\r\n]*Basic[ \t\r\n]+realm[ \t\r\n]*=[ \t\r\n]*"
				"[~ \t\r\n,]*",
			NULL, NULL, NULL, NULL, &basic->realm) || !pl_isset(&basic->realm))
		return EBADMSG;

	return 0;
}


int httpauth_basic_make_response(struct httpauth_basic *basic,
		const struct pl *user, const struct pl *pwd)
{
	uint8_t *in;
	char *out;
	size_t si, so;
	int err;

	if (!basic || !basic->mb || !user || !pwd)
		return EINVAL;

	si = user->l + pwd->l + 1;
	so = 4 * (si + 2) / 3;
	err = mbuf_resize(basic->mb, si + so + 1 + 21);
	if (err)
		return err;

	in = mbuf_buf(basic->mb);
	err = mbuf_printf(basic->mb, "%r:%r", user, pwd);

	out = (char*) mbuf_buf(basic->mb);
	err |= mbuf_fill(basic->mb, 0, so + 1);
	if (err)
		goto fault;

	err = base64_encode(in, si, out, &so);
	if (err)
		goto fault;

	pl_set_str(&basic->auth, out);

	return 0;

fault:
	mem_deref(basic->mb);
	return err;
}

int httpauth_basic_encode(const struct httpauth_basic *basic, struct mbuf *mb)
{
	int err;

	if (!basic || !mb || !pl_isset(&basic->auth))
		return EINVAL;

	err = mbuf_resize(mb, basic->auth.l + 21);
	if (err)
		return err;

	err = mbuf_write_str(mb, "Authorization: Basic ");
	err |= mbuf_write_pl(mb, &basic->auth);
	if (err)
		return err;

	mbuf_set_pos(mb, 0);
	return 0;
}

