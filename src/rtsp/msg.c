/**
 * @file rtsp/msg.c  RTSP Message decode
 *
 * Copyright (C) 2019 Christoph Huber
 */

#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_msg.h>
#include <re_rtsp.h>

#define DEBUG_MODULE "rtsp_msg_decode"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
    STARTLINE_MAX = 8192
};


static void hdr_destructor(void *arg)
{
    struct rtsp_hdr *hdr = arg;

    list_unlink(&hdr->le);
}


static void destructor(void *arg)
{
    struct rtsp_msg *msg = arg;

    list_flush(&msg->hdrl);
    mem_deref(msg->_mb);
    mem_deref(msg->mb);
}


static enum rtsp_hdrid hdr_hash(const struct pl *name)
{
    if (!name->l)
        return RTSP_HDR_NONE;

    return (enum rtsp_hdrid)(hash_joaat_ci(name->p, name->l) & 0xfff);
}


static inline bool hdr_comma_separated(enum rtsp_hdrid id)
{
    switch (id){
    case RTSP_HDR_ACCEPT:
    case RTSP_HDR_ACCEPT_ENCODING:
    case RTSP_HDR_ACCEPT_LANGUAGE:
    case RTSP_HDR_ALLOW:
    case RTSP_HDR_CACHE_CONTROL:
    case RTSP_HDR_CONNECTION:
    case RTSP_HDR_CONTENT_ENCODING:
    case RTSP_HDR_CONTENT_LANGUAGE:
    case RTSP_HDR_PUBLIC:
    case RTSP_HDR_RTP_INFO:
    case RTSP_HDR_TRANSPORT:
    case RTSP_HDR_VIA:
        return true;

    default:
        return false;
    }
}


static inline int hdr_add(struct rtsp_msg *msg, const struct pl *name,
    enum rtsp_hdrid id, const char *p, ssize_t l)
{
    struct rtsp_hdr *hdr;
    int err = 0;

    hdr = mem_zalloc(sizeof(*hdr), hdr_destructor);
    if (!hdr)
    return ENOMEM;

    hdr->name = *name;
    hdr->val.p = p;
    hdr->val.l = MAX(l, 0);
    hdr->id = id;

    list_append(&msg->hdrl, &hdr->le, hdr);
    switch(id) {
        case RTSP_HDR_CONTENT_TYPE:
            err = msg_ctype_decode(&msg->ctype, &hdr->val);
            break;

        case RTSP_HDR_CONTENT_LENGTH:
            msg->clen = pl_u32(&hdr->val);
            break;

        case RTSP_HDR_CSEQ:
            msg->cseq = pl_u32(&hdr->val);
            break;

        default:
            break;
    }

    if (err)
        mem_deref(hdr);

    return err;
}


/**
 * Decode a RTSP message
 *
 * @param msgp Pointer to allocated RTSP Message
 * @param mb   Buffer containing RTSP Message
 * @param req  True for server call, false for client call
 *
 * @return 0 if success, otherwise errorcode
 */
int rtsp_msg_decode(struct rtsp_msg **msgp, struct mbuf *mb, bool svr)
{
    struct pl b, s, e, name, scode;
    const char *p, *cv;
    bool comsep, quote, res = true;
    struct rtsp_msg *msg;
    enum rtsp_hdrid id = RTSP_HDR_NONE;
    uint32_t ws, lf;
    size_t l;
    int err;

    if (!msgp || !mb)
        return EINVAL;

    msg = mem_zalloc(sizeof(*msg), destructor);
    if (!msg)
        return ENOMEM;

    msg->_mb = mem_ref(mb);

    if (!svr) {
        msg->mb = mbuf_alloc(8192);
        if (!msg->mb) {
            err = ENOMEM;
            goto out;
        }
    }

  /*ILD - Detection*/
    if (mbuf_read_u8(msg->_mb) == 0x24) {
        if (mbuf_get_left(msg->_mb) < 3) {
            mbuf_advance(msg->_mb, -1);
            err = ENODATA;
            goto out;
        }

        msg->mtype = RTSP_ILD;
        msg->channel = mbuf_read_u8(msg->_mb);
        msg->clen = ntohs(mbuf_read_u16(msg->_mb));
        mbuf_advance(msg->_mb, -4);
        l = msg->clen;
        err = 0;
        goto out;
    } else {
        mbuf_advance(msg->_mb, -1);
    }

    p = (const char *)mbuf_buf(mb);
    l = mbuf_get_left(mb);
    if (re_regex(p, l, "[\r\n]*[^\r\n]+[\r]*[\n]1", &b, &s, NULL, &e)) {
        err = (l > STARTLINE_MAX) ? EBADMSG : ENODATA;
        goto out;
    }

    /*RES - Detection */
    if (re_regex(s.p, s.l, "RTSP/[0-9.]+ [0-9]+[ ]*[^]*",
        &msg->ver, &scode, NULL, &msg->reason) || msg->ver.p != s.p + 5) {
        res = false;
    } else {
        msg->scode = pl_u32(&scode);
        msg->mtype = RTSP_RESPONSE;
        res = true;
    }

    /* REQ - Detection*/
    if (!res) {
        if (re_regex(s.p, s.l, "[a-z|_]+ [^? ]+[^ ]* RTSP/[0-9.]+",
            &msg->met, &msg->path, &msg->prm, &msg->ver) || msg->met.p != s.p) {
            err = EBADMSG;
            goto out;
        }
        msg->mtype = RTSP_REQUEST;
    }

    l -= e.p + e.l - p;
    p = e.p + e.l;

    name.p = cv = NULL;
    name.l = ws = lf = 0;
    comsep = false;
    quote = false;

    for (; l > 0; p++, l--) {
        switch (*p) {
            case ' ':
            case '\t':
                lf = 0; /* folding */
                ++ws;
                break;

            case '\r':
                ++ws;
                break;

            case '\n':
                ++ws;
                if (!name.p) {
                    ++p; --l; /* no headers */
                    err = 0;
                    goto out;
                }

                if (!lf++)
                    break;

                ++p; --l; /* eoh */

            /*@fallthrough@*/
            default:
                if (lf || (*p == ',' && comsep && !quote)) {
                    if (!name.l) {
                        err = EBADMSG;
                        goto out;
                    }

                    err = hdr_add(msg, &name, id, cv ? cv : p,
                        cv ? p - cv - ws : 0);
                    if (err)
                    goto out;

                    if (!lf) { /* comma separated */
                        cv = NULL;
                        break;
                    }

                    if (lf > 1) { /* eoh */
                        err = 0;
                        goto out;
                    }

                    comsep = false;
                    name.p = NULL;
                    cv = NULL;
                    lf = 0;
                }

                if (!name.p) {
                    name.p = p;
                    name.l = 0;
                    ws = 0;
                }

                if (!name.l) {
                    if (*p != ':') {
                        ws = 0;
                        break;
                    }

                    name.l = MAX((int)(p - name.p - ws), 0);
                    if (!name.l) {
                        err = EBADMSG;
                        goto out;
                    }

                    id = hdr_hash(&name);
                    comsep = hdr_comma_separated(id);
                    break;
                }

                if (!cv) {
                    quote = false;
                    cv = p;
                }

                if (*p == '"')
                    quote = !quote;

                ws = 0;
                break;
        }
    }

    err = ENODATA;

  out:
    if (err) {
        mem_deref(msg);
    } else {
        *msgp = msg;
        mb->pos = mb->end - l;
    }

    return err;
}


/**
 * get a header field by id
 *
 * @param msg     Pointer to the RTSP message
 * @param id      Unique header id
 *
 * @return        at success pointer to header field, NULL otherwise
 */
const struct rtsp_hdr *rtsp_msg_hdr(const struct rtsp_msg *msg,
    enum rtsp_hdrid id)
{
    return rtsp_msg_hdr_apply(msg, true, id, NULL, NULL);
}


/**
 * header list apply function
 *
 * @param msg     Pointer to the RTSP message
 * @param fwd     true: list forward / false: list backward
 * @param id      Unique header id
 * @param h       Handler function which should be applied
 * @param arg     Argument for the handler function
 *
 * @retun         return value from @h at success, NULL otherwise
 */
const struct rtsp_hdr *rtsp_msg_hdr_apply(const struct rtsp_msg *msg,
    bool fwd, enum rtsp_hdrid id, rtsp_hdr_h *h, void *arg)
{
    struct le *le;

    if (!msg)
        return NULL;

    le = fwd ? msg->hdrl.head : msg->hdrl.tail;
    while (le) {
        const struct rtsp_hdr *hdr = le->data;
        le = fwd ? le->next : le->prev;

        if (hdr->id != id)
            continue;

        if (!h || h (hdr, arg))
            return hdr;
    }

    return NULL;
}


static bool count_handler(const struct rtsp_hdr *hdr, void *arg)
{
    uint32_t *n = arg;

    (void)hdr;

    ++(*n);
    return false;
}


static bool value_handler(const struct rtsp_hdr *hdr, void *arg)
{
    return 0 == pl_strcasecmp(&hdr->val, (const char*) arg);
}


/**
 * Checks the value of a header field
 *
 * @param msg     Pointer to RTSP message
 * @param id      Unique header id
 * @param value   Value to compare
 *
 * @return        true / false
 */
bool rtsp_msg_hdr_has_value(const struct rtsp_msg *msg, enum rtsp_hdrid id,
    const char *value)
{
    return NULL != rtsp_msg_hdr_apply(msg, true, id, value_handler,
        (void *) value);
}


/**
 * Count the available header elements
 *
 * @param msg     Pointer to RTSP message
 * @param id      Unique header id
 *
 * @return        Number of available headers
 */
uint32_t rtsp_msg_hdr_count(const struct rtsp_msg *msg, enum rtsp_hdrid id)
{
    uint32_t n = 0;

    rtsp_msg_hdr_apply(msg, true, id, count_handler, &n);
    return n;
}


/**
 * Print a RTSP Message
 *
 * @param pf    Print function for output
 * @param msg   RTSP Message
 *
 * @return      0 if success, otherwise errorcode
 */
int rtsp_msg_print(struct re_printf *pf, const struct rtsp_msg *msg)
{
    struct le *le;
    int err;

    if (!msg)
    return 0;

    if (pl_isset(&msg->met))
        err = re_hprintf(pf, "%r %r %r RTSP/%r\n", &msg->met, &msg->path,
            &msg->prm, &msg->ver);
    else
        err = re_hprintf(pf, "RTSP/%r %u %r\n", &msg->ver,
            msg->scode, &msg->reason);

    for (le = msg->hdrl.head; le; le = le->next) {
        const struct rtsp_hdr *hdr = le->data;
        err |= re_hprintf(pf, "%r: %r (%i)\n", &hdr->name, &hdr->val, hdr->id);
    }

    return err;
}