/**
 * @file xml.c XML Parser
 *
 * Copyright (C) 2018
 */

#include "xml.h"
#include <re_xml.h>

/*LATER!*/
/*TODO: The XML values and keys are UTF-8 encoded. Therefore i need an encoder
	aswell as a decoder for the UTF-8 character set.
	The begin and end character of a XML message is the '<' and
	the '</' charater/string. Both are in the ascii code encoding. Searching
	for these elements can be done with reading byte by byte.
*/

/*TODO: UTF-8 encoder to read the correct amount of bytes depending on
	the character used: Read the first byte of the character.
	0xxxxxxx - one	 byte
	110xxxxx - two	 bytes
	1110xxxx - three bytes
	11110xxx - four  bytes
	10xxxxxx - continuing bytes

	Counting the bytes needen for a utf-8 encoded string in the memory buffer
	is to count every continouing byte (starts with 10xxxxxx)
	*/

/**
 * find the next key in the XML data
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_next_key(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			continue;
		}

		if (symbol == '<' && !quote)
			break;
	}

	return error;
}

/**
 * find the previous key in the XML data
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_prev_key(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	if (buf->pos == 1)
		return EOVERFLOW;

	pos = buf->pos;
	while (!error) {
		if (!buf->pos) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		mbuf_advance(buf, -1);
		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			mbuf_advance(buf, -1);
			continue;
		}

		if (symbol == '<' && !(pos == buf->pos) && !quote)
			break;

		mbuf_advance(buf, -1);
	}

	return error;
}

/**
 * Skip all characters until and including the given character c.
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
static int xml_skip_to(struct mbuf *buf, const char c)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			continue;
		}

		if (symbol == c && !quote)
			break;
	}

	return error;
}

/**
 * skip all parameter of the element till and including the end symbol '>'
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_skip_to_end(struct mbuf *buf)
{
	return xml_skip_to(buf, '>');
}

/**
 * skip all parameter of the element till the symbol '<'
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_skip_to_begin(struct mbuf *buf)
{
	size_t bpos = buf->pos;

	int err = xml_skip_to(buf, '<');
	if (!err)
		mbuf_advance(buf, -1);
	else
		mbuf_set_pos(buf, bpos);

	return err;
}


/**
 * skip all parameter of the element till a WS
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_skip_to_ws(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			continue;
		}

		if ((symbol == '>') && !quote) {
			mbuf_set_pos(buf, pos);
			return EOF;
		}

		if ((symbol == ' ') && !quote)
			break;
	}

	return error;
}

/**
 * test the current key as close key ("</")
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 * @param ck			 pointer to store the result of the check
 *
 * @return				 0 if success, error code otherwise
 */
int xml_is_close_key(struct mbuf *buf, bool *ck)
{
	if (!buf || !ck)
		return EINVAL;

	if (!mbuf_get_left(buf))
		return EOVERFLOW;

	if (mbuf_read_u8(buf) == '/') {
		*ck = true;
	} else {
		*ck = false;
	}
	mbuf_advance(buf, -1);

	return 0;
}


/**
 * test the current key as start AND close key ("<xxxx />")
 * @param buf			pointer to the xml data in form of @mbuf struct
 * @param ck			pointer to store the result of the check
 *
 * @return				0 if success, error code otherwise
 */
int xml_is_startclose_key(struct mbuf *buf, bool *ck)
{
	int err = 0;

	if (!buf || !ck)
		return EINVAL;

	if (!mbuf_get_left(buf))
		return EOVERFLOW;

	err = xml_skip_to_end(buf);
	if (err)
		return err;

	mbuf_advance(buf, -2);
	if (mbuf_read_u8(buf) == '/') {
		*ck = true;
	} else {
		*ck = false;
	}

	mbuf_advance(buf, 1);
	return 0;
}


/**
 * find a child of the current key in the XML data
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_get_child(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool ck = false;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	error = xml_next_key(buf);
	if (error)
		return error;

	error = xml_is_close_key(buf, &ck);
	if (error)
		goto OUT;

	if (ck) {
		error = EOVERFLOW;
		goto OUT;
	}

OUT:
	if (error)
		mbuf_set_pos(buf, pos);

	return error;
}

/**
 * find a parent of the current key in the XML data
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_get_parent(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool ck = false;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	error = xml_prev_key(buf);
	if (error)
		return error;

	error = xml_is_close_key(buf, &ck);
	if (error)
		goto OUT;

	if (ck) {
		error = EOVERFLOW;
		goto OUT;
	}

OUT:
	if (error)
		mbuf_set_pos(buf, pos);

	return error;
}

/**
 * compare the current XML key with a given string and
 * a number of bytes to compare
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 * @param cmp			 compare string
 * @param n				 number of bytes to compare
 *
 * @return				 0 for match, anything else for missmatch
 */
int xml_cmp_key(struct mbuf *buf, const char *cmp, size_t n)
{
	bool ck;
	int error = 0;
	int cmp_res = 0;
	size_t pos = 0;
	size_t pos_end = 0;
	size_t pos_ws = 0;

	if (!buf || !cmp)
		return EINVAL;

	pos = buf->pos;
	error = xml_is_close_key(buf, &ck);
	if (error)
		return error;

	if (ck)
		mbuf_advance(buf, 1);

	error = xml_skip_to_end(buf);
	if (error)
		return error;

	pos_end = buf->pos - 1;
	mbuf_set_pos(buf, pos);
	error = xml_skip_to_ws(buf);
	if (!error)
		pos_ws = buf->pos - 1;
	else if (error == EOF)
		pos_ws = pos_end;
	else
		return error;

	mbuf_set_pos(buf, pos);

	if (!ck) {
		if ((pos_end - pos) != n && (pos_ws - pos) != n)
			return EBADMSG;

		cmp_res = memcmp(mbuf_buf(buf), cmp, n);
	}
	else {
		if ((pos_end - pos - 1) != n )
			return EBADMSG;

		cmp_res = memcmp(mbuf_buf(buf) + 1, cmp, n);
	}

	return cmp_res;
}

/**
 * Skip the XML prolog if available.
 * @param buf		 pointer to the xml data in the form of a @mbuf struct
 *
 * @return			 0 if success, error code otherwise
 */
int xml_skip_prolog(struct mbuf *buf)
{
	int error = 0;
	size_t bpos;

	if (!buf)
		return EINVAL;

	mbuf_set_pos(buf, 0);
	xml_skip_to_begin(buf);
	bpos = buf->pos;

	if (mbuf_read_u8(buf) == '<' && mbuf_read_u8(buf) == '?') {
		error = xml_skip_to_end(buf);
		if (error)
			return error;
	} else {
		mbuf_set_pos(buf, bpos);
	}

	return 0;
}

/**
 * set the buffer position to the beginning of the next parameter
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_next_param(struct mbuf *buf)
{
	int error = 0;

	if (!buf)
		return EINVAL;

	error = xml_skip_to_ws(buf);
	if (error)
		return error;

	return 0;
}

/**
 * set the buffer position to the beginning of the previous parameter
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_prev_param(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	if (buf->pos == 1)
		return EOVERFLOW;

	pos = buf->pos;
	while (!error) {
		if (!buf->pos) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		mbuf_advance(buf, -1);
		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			mbuf_advance(buf, -1);
			continue;
		}

		if ((symbol == '<' || symbol == '>' || symbol == '/') &&
				!(pos == buf->pos) && !quote) {
			mbuf_set_pos(buf, pos);
			return EOF;
		}

		if (symbol == ' ' && !(pos == buf->pos) && !quote)
			break;

		mbuf_advance(buf, -1);
	}

	return error;
}

/**
 * set the position of the @mbuf buffer to the first element after a '='
 * in the parameter list.
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_goto_value(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	bool quote = false;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			quote = !quote;
			continue;
		}

		if (symbol == '=' && !quote)
			break;
	}

	return error;
}

/**
 * set the position of the @mbuf buffer to the first character after next quote
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_goto_value_begin(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'')
			break;
	}

	return error;
}

/**
 * set the position of the @mbuf buffer to the next quote
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_goto_value_end(struct mbuf *buf)
{
	int error = 0;
	size_t pos = 0;
	char symbol;

	if (!buf)
		return EINVAL;

	pos = buf->pos;
	while (!error) {
		if (!mbuf_get_left(buf)) {
			mbuf_set_pos(buf, pos);
			return EOVERFLOW;
		}

		symbol = mbuf_read_u8(buf);
		if (symbol == '\"' || symbol == '\'') {
			if (buf->pos > 0)
				mbuf_advance(buf, -1);
			else
				return EINVAL;
			break;
		}
	}

	return error;
}

/*----------------------------------------------------------------------------*/
/**
 * write the standard XML header line into @buf
 * this function will change the size of the memory buffer if necessary
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 *
 * @return				 0 if success, error code otherwise
 */
int xml_add_prolog(struct mbuf *buf)
{
	int error = 0;
	const char xml_prolog [] = "<?xml version=\"1.0\"?>\n";

	if (!buf || !(buf->pos == 0))
		return EINVAL;

	if (mbuf_get_space(buf) < strlen(xml_prolog)) {
		error = mbuf_resize(buf, (strlen(xml_prolog) + XML_BUFFER_GROWTH + buf->size));
		if (error)
			return error;
	}

	error = mbuf_write_mem(buf, (uint8_t*)xml_prolog, sizeof(xml_prolog) - 1);

	return error;
}

/**
 * write a key element @key with size @n into the XML buffer @buf
 * this function will change the size of the memory buffer if necessary
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 * @param key			 pointer to the key string
 *
 * @return				 0 if success, error code otherwise
 */
int xml_add_key(struct mbuf *buf, const struct pl *key)
{
	int error = 0;
	int min_growth = 0;

	if (!buf || !key)
		return EINVAL;

	if (mbuf_get_space(buf) < (key->l + XML_KEY_SYMBOLS)) {
		min_growth = (key->l + XML_KEY_SYMBOLS) > XML_BUFFER_GROWTH ? key->l + 2 :
									XML_BUFFER_GROWTH;
		error = mbuf_resize(buf, (buf->size + min_growth));
		if (error)
			return error;
	}

	error = mbuf_write_u8(buf, '<');
	error |= mbuf_write_pl(buf, key);
	error |= mbuf_write_u8(buf, '>');

	return error;
}

/**
 * write a key element @key with size @kn into the XML buffer @buf
 * additionaly write a @param and possible @value to the @key element
 * this function will change the size of the memory buffer if necessary
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 * @param key			 pointer to the key string
 * @param param		 pointer to the param string
 * @param value		 pointer to the value string
 *
 * @return				 0 if success, error code otherwise
 */
int xml_add_key_param(struct mbuf *buf, const struct pl *key,
	const struct pl *param, const struct pl *value)
{
	int error = 0;
	int min_growth = 0;
	size_t size;

	if (!buf || !key || !param)
		return EINVAL;

	size = key->l + param->l + value->l + XML_KEY_SYMBOLS
		+ XML_V_SYMBOLS(value->l);
	if (mbuf_get_space(buf) < size) {
		min_growth = size > XML_BUFFER_GROWTH ? size : XML_BUFFER_GROWTH;
		error = mbuf_resize(buf, (buf->size + min_growth));
		if (error)
			return error;
	}

	error = mbuf_write_u8(buf, '<');
	error |= mbuf_write_pl(buf, key);
	error |= mbuf_write_u8(buf, ' ');
	error |= mbuf_write_pl(buf, param);
	if (value) {
		error |= mbuf_write_u8(buf, '=');
		error |= mbuf_write_pl(buf, value);
	}

	error |= mbuf_write_u8(buf, '>');

	return error;
}


/**
 * write a close key element @key with size @n into the XML buffer @buf
 * this function will change the size of the memory buffer if necessary
 * @param buf			 pointer to the xml data in form of a @mbuf struct
 * @param key			 pointer to the key string
 *
 * @return				 0 if success, error code otherwise
 */
int xml_add_ckey(struct mbuf *buf, const struct pl *key)
{
	int error = 0;
	int min_growth = 0;

	if (!buf || !key)
		return EINVAL;

	if (mbuf_get_space(buf) < (key->l + XML_CKEY_SYMBOLS)) {
		min_growth = (key->l + XML_CKEY_SYMBOLS) > XML_BUFFER_GROWTH ? key->l + 2
									: XML_BUFFER_GROWTH;
		error = mbuf_resize(buf, (buf->size + min_growth));
		if (error)
			return error;
	}

	error = mbuf_write_u8(buf, '<');
	error |= mbuf_write_u8(buf, '/');
	error |= mbuf_write_pl(buf, key);
	error |= mbuf_write_u8(buf, '>');

	return error;
}

