/**
 * @file re_rtsp.h  Real-Time Streaming Protocol
 *
 * Copyright (C) 2019 Christoph Huber
 */

enum rtsp_hdrid {
    /*GENERAL REQUEST HDRID*/
    RTSP_HDR_ACCEPT_RANGES = 3027, /*RTSP 2.0*/
    RTSP_HDR_CACHE_CONTROL = 2530,
    RTSP_HDR_CONNECTION = 865,
    RTSP_HDR_CONNECTION_CREDENTIALS = 454, /*RTSP 2.0*/
    RTSP_HDR_CSEQ = 746,
    RTSP_HDR_DATE = 1027,
    RTSP_HDR_PIPELINED_REQUEST = 40, /*RTSP 2.0*/
    RTSP_HDR_VIA = 3961,

    /*REQUEST HDRID*/
    RTSP_HDR_ACCEPT = 3186,
    RTSP_HDR_ACCEPT_CREDENTIALS = 302, /*RTSP 2.0*/
    RTSP_HDR_ACCEPT_ENCODING = 708,
    RTSP_HDR_ACCEPT_LANGUAGE = 2867,
    RTSP_HDR_AUTHORIZATION = 2503,
    RTSP_HDR_BANDWIDTH = 3513,
    RTSP_HDR_BLOCKSIZE = 642,
    RTSP_HDR_CONFERENCE = 3885,
    RTSP_HDR_FROM = 1963,
    RTSP_HDR_IF_MATCH = 2684, /*RTSP 2.0*/
    RTSP_HDR_IF_MODIFIED_SINCE = 2187,
    RTSP_HDR_IF_NONE_MATCH = 4030,       /*RTSP 2.0*/
    RTSP_HDR_PROXY_AUTHORIZATION = 2363, /*RTSP 2.0*/
    RTSP_HDR_PROXY_REQUIRE = 3562,
    RTSP_HDR_REFERRER = 2991,
    RTSP_HDR_REQUEST_STATUS = 96, /*RTSP 2.0*/
    RTSP_HDR_REQUIRE = 3905,
    RTSP_HDR_SEEK_STYLE = 4070,       /*RTSP 2.0*/
    RTSP_HDR_SUPPORTED = 119,         /*RTSP 2.0*/
    RTSP_HDR_TERMINATE_REASON = 3889, /*RTSP 2.0*/
    RTSP_HDR_TIMESTAMP = 938,         /*RTSP 2.0*/
    RTSP_HDR_USER_AGENT = 4064,

    /*RESPONSE HDRID*/
    RTSP_HDR_ALLOW = 2429,
    RTSP_HDR_AUTHENTICATION_INFO = 3144, /*RTSP 2.0*/
    RTSP_HDR_LOCATION = 2514,            /*RTSP 2.0*/
    RTSP_HDR_MEDIA_PROPERTIES = 2451,    /*RTSP 2.0*/
    RTSP_HDR_MEDIA_RANGE = 2814,         /*RTSP 2.0*/
    RTSP_HDR_MTAG = 2751,                /*RTSP 2.0*/
    RTSP_HDR_PUBLIC = 2668,
    RTSP_HDR_RETRY_AFTER = 409,
    RTSP_HDR_RTP_INFO = 853,
    RTSP_HDR_PROXY_AUTHENTICATION_INFO = 3538, /*RTSP 2.0*/
    RTSP_HDR_PROXY_SUPPORTED = 296,            /*RTSP 2.0*/
    RTSP_HDR_SERVER = 2752,
    RTSP_HDR_UNSUPPORTED = 982,
    RTSP_HDR_WWW_AUTHENTICATE = 2763,

    /*ENTITY HDRID*/
    RTSP_HDR_CONTENT_BASE = 3970,
    RTSP_HDR_CONTENT_ENCODING = 580,
    RTSP_HDR_CONTENT_LANGUAGE = 3371,
    RTSP_HDR_CONTENT_LENGTH = 3861,
    RTSP_HDR_CONTENT_LOCATION = 3927,
    RTSP_HDR_CONTENT_TYPE = 809,
    RTSP_HDR_EXPIRES = 1983,
    RTSP_HDR_LAST_MODIFIED = 2946,

    /*REST*/
    RTSP_HDR_PROXY_AUTHENTICATE = 116,
    RTSP_HDR_RANGE = 4004,
    RTSP_HDR_SCALE = 3292,
    RTSP_HDR_SESSION = 1931,
    RTSP_HDR_SPEED = 555,
    RTSP_HDR_TRANSPORT = 673,

    RTSP_HDR_NONE = -1
};

enum rtsp_msgtype {
    RTSP_REQUEST,
    RTSP_RESPONSE,
    RTSP_ILD,

    RTSP_TYPE_NONE = -1
};


struct rtsp_hdr {
    struct le le;                   /**< List element          */
    struct pl name;                 /**< Header name           */
    struct pl val;                  /**< Header value          */
    enum rtsp_hdrid id;             /**< Header unique id      */
};

struct rtsp_msg {
    struct le le;                   /**< List element          */
    struct pl ver;                  /**< RTSP version          */
    struct pl met;                  /**< RTSP Request method   */
    struct pl path;                 /**< Resource path         */
    struct pl prm;                  /**< Parameter             */
    uint16_t scode;                 /**< RTSP status code      */
    uint32_t cseq;                  /**< RTSP csequence        */
    struct pl reason;               /**< RTSP reason           */
    struct list hdrl;               /**< Header list           */
    struct msg_ctype ctype;         /**< Content type          */
    struct mbuf *_mb;               /**< Message buffer        */
    struct mbuf *mb;                /**< Message body buffer   */
    uint32_t clen;                  /**< Content length        */
    uint8_t channel;                /**< ILD Channel           */
    enum rtsp_msgtype mtype;        /**< Message type          */
};


typedef bool(rtsp_hdr_h)(const struct rtsp_hdr *hdr, void *arg);

int rtsp_msg_decode(struct rtsp_msg **msgp, struct mbuf *mb, bool svr);

const struct rtsp_hdr *rtsp_msg_hdr(const struct rtsp_msg *msg,
    enum rtsp_hdrid id);
const struct rtsp_hdr *rtsp_msg_hdr_apply(const struct rtsp_msg *msg,
    bool fwd, enum rtsp_hdrid id, rtsp_hdr_h *h, void *arg);
int rtsp_msg_print(const struct rtsp_msg *msg);

uint32_t rtsp_msg_hdr_count(const struct rtsp_msg *msg, enum rtsp_hdrid id);
bool rtsp_msg_hdr_has_value(const struct rtsp_msg *msg, enum rtsp_hdrid id,
    const char *value);


/* Server */
struct rtsp_sock;
struct rtsp_conn;

typedef void(rtsp_sock_msg_h)(struct rtsp_conn *conn,
    const struct rtsp_msg *msg, void *arg);

int rtsp_listen(struct rtsp_sock **sockp, const struct sa *laddr,
    rtsp_sock_msg_h sockmsgh, void *arg);
int rtsps_listen(struct rtsp_sock **sockp, const struct sa *laddr,
    const char *cert, rtsp_sock_msg_h sockmsgh, void *arg);


struct tcp_sock *rtsp_sock_tcp(const struct rtsp_sock *sock);
const struct sa *rtsp_conn_peer(const struct rtsp_conn *conn);
struct tcp_conn *rtsp_conn_tcp(const struct rtsp_conn *conn);
struct tls_conn *rtsp_conn_tls(const struct rtsp_conn *conn);
void rtsp_conn_close(struct rtsp_conn *conn);

int rtsp_reply(const struct rtsp_conn *conn, uint8_t ver, uint16_t scode,
    const char *reason, const char *fmt, ...);
int rtsp_creply(const struct rtsp_conn *conn, uint8_t ver, uint16_t scode,
    const char *reason, const char *ctype, struct mbuf *data,
    const char *fmt, ...);
int rtsp_send_ild(struct rtsp_conn *conn, uint8_t ch, uint8_t *data, size_t n);
int rtsp_send_req(struct rtsp_msg **msgp, struct rtsp_conn *conn, uint8_t ver,
    const char *method, const char *path, const char *fmt, ...);
int rtsp_send_creq(struct rtsp_msg **msgq, struct rtsp_conn *conn, uint8_t ver,
    const char *method, const char *path, const char *ctype, struct mbuf *data,
    const char *fmt, ...);