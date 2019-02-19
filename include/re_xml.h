/* @file re_xml.h  XML
 *
 * Copyright (C) 2018
 */

#ifndef _RE_XML_H_
#define _RE_XML_H_

#define XML_BUFFER_GROWTH 50
#define XML_KEY_SYMBOLS 2
#define XML_CKEY_SYMBOLS 3
/* XML_V_SYMBOLS are one '=' and two '"' */
#define XML_V_SYMBOLS(x) x ? 3 : 0

int xml_next_key(struct mbuf *buf);
int xml_prev_key(struct mbuf *buf);
int xml_skip_to_begin(struct mbuf *buf);
int xml_skip_to_end(struct mbuf *buf);
int xml_skip_to_ws(struct mbuf *buf);
int xml_is_close_key(struct mbuf *buf, bool *ck);
int xml_is_startclose_key(struct mbuf *buf, bool *ck);

int xml_get_child(struct mbuf *buf);
int xml_get_parent(struct mbuf *buf);
int xml_cmp_key(struct mbuf *buf, const char *cmp, size_t n);
int xml_skip_prolog(struct mbuf *buf);

int xml_next_param(struct mbuf *buf);
int xml_prev_param(struct mbuf *buf);
int xml_goto_value(struct mbuf *buf);
int xml_goto_value_begin(struct mbuf *buf);
int xml_goto_value_end(struct mbuf *buf);

int xml_add_prolog(struct mbuf *buf);
int xml_add_key(struct mbuf *buf, const struct pl *key);
int xml_add_key_param(struct mbuf *buf, const struct pl *key,
		const struct pl *param, const struct pl *value);
int xml_add_ckey(struct mbuf *buf, const struct pl *key);



#endif /* _RE_XML_H_ */

