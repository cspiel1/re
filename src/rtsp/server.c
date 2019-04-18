/**
 * @file rtsp/server.c RTSP Server
 *
 * Copyright (C) 2019 Christoph Huber
 */

#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_fmt.h>
#include <re_tmr.h>
#include <re_srtp.h>
#include <re_tcp.h>
#include <re_tls.h>
#include <re_msg.h>
#include <re_rtsp.h>

#define DEBUG_MODULE "rtsp_server"
#define DEBUG_LEVEL 6
#include <re_dbg.h>

enum
{
	TIMEOUT_IDLE = 600000,
	TIMEOUT_INIT = 10000,
	BUFSIZE_MAX = 524288,
};


struct rtsp_sock
{
	struct list connl;
	struct tcp_sock *ts;
	struct tls *tls;
	rtsp_sock_msg_h *sockmsgh;
	void *arg;
};

struct rtsp_conn
{
	struct le le;
	struct tmr tmr;
	struct sa peer;
	struct rtsp_sock *sock;
	struct tcp_conn *tc;
	struct tls_conn *sc;
	struct mbuf *mb;
};


static void conn_close(struct rtsp_conn *conn);


static void sock_destructor(void *arg)
{
	struct rtsp_sock *sock = arg;
	struct le *le;

	le = sock->connl.head;
	while (le){
		struct rtsp_conn *conn = le->data;
		le = le->next;

		conn_close(conn);
		mem_deref(conn);
	}

	mem_deref(sock->tls);
	mem_deref(sock->ts);
}


static void conn_destructor(void *arg)
{
	struct rtsp_conn *conn = arg;

	list_unlink(&conn->le);
	tmr_cancel(&conn->tmr);
	mem_deref(conn->sc);
	mem_deref(conn->tc);
	mem_deref(conn->mb);
}


static void conn_close(struct rtsp_conn *conn)
{
	list_unlink(&conn->le);
	tmr_cancel(&conn->tmr);
	conn->sc = mem_deref(conn->sc);
	conn->tc = mem_deref(conn->tc);
	conn->sock = NULL;
}


/**
 * rtsp->tcp timeout handler
 *
 * gets called at a connection timeout
 */
static void timeout_handler(void *arg)
{
	struct rtsp_conn *conn = arg;

	conn_close(conn);
	mem_deref(conn);
}


/*
 * rtsp->tcp receive handler
 *
 * if exactly a full package is received -> everything fine
 * if received data < full package -> wait for more data
 * if received data > full package -> process data and maybe wait for more
 */
static void recv_handler(struct mbuf *mb, void *arg)
{
	struct rtsp_conn *conn = arg;
	int err = 0;

	if (conn->mb) {
		const size_t len = mbuf_get_left(mb), pos = conn->mb->pos;

		if ((mbuf_get_left(conn->mb) + len) > BUFSIZE_MAX) {
			err = EOVERFLOW;
			goto out;
		}

		conn->mb->pos = conn->mb->end;
		err = mbuf_write_mem(conn->mb, mbuf_buf(mb), len);
		if (err)
			goto out;

		conn->mb->pos = pos;
	}
	else {
		conn->mb = mem_ref(mb);
	}

	while (conn->mb) {
		size_t end, pos = conn->mb->pos;
		struct rtsp_msg *msg;

		err = rtsp_msg_decode(&msg, conn->mb, true);
		if (err) {
			if (err == ENODATA) {
				conn->mb->pos = pos;
				err = 0;
				break;
			}

			goto out;
		}

		if (mbuf_get_left(conn->mb) < msg->clen) {
			conn->mb->pos = pos;
			mem_deref(msg);
			break;
		}

		msg->mb = mem_ref(msg->_mb);
		mb = conn->mb;
		end = mb->end;
		mb->end = mb->pos + msg->clen;
		if (end > mb->end) {
			struct mbuf *mbn = mbuf_alloc(end - mb->end);
			if (!mbn) {
				mem_deref(msg);
				err = ENOMEM;
				goto out;
			}

			(void)mbuf_write_mem(mbn, mb->buf + mb->end, end - mb->end);
			mbn->pos = 0;
			mem_deref(conn->mb);
			conn->mb = mbn;
		}
		else {
			conn->mb = mem_deref(conn->mb);
		}

		if (conn->sock)
			conn->sock->sockmsgh(conn, msg, conn->sock->arg);

		mem_deref(msg);

		if (!conn->tc) {
			err = ENOTCONN;
			goto out;
		}

		tmr_start(&conn->tmr, TIMEOUT_IDLE, timeout_handler, conn);
	}

 out:
	if (err) {
		conn_close(conn);
		mem_deref(conn);
	}
}


/**
 * rtsp->tcp close handler
 *
 * gets called if a connection is closed by the peer
 */
static void close_handler(int err, void *arg)
{
	struct rtsp_conn *conn = arg;

	if (err)
		DEBUG_WARNING("%s Connection close on err=(%m)", __func__, err);

	conn_close(conn);
	mem_deref(conn);
}


/**
 * rtsp->tcp connection handler
 *
 * gets called at a incomming tcp connection
 */
static void connect_handler(const struct sa *peer, void *arg)
{
	struct rtsp_sock *sock = arg;
	struct rtsp_conn *conn;
	int err;

	conn = mem_zalloc(sizeof(*conn), conn_destructor);
	if (!conn) {
		err = ENOMEM;
		goto out;
	}

	list_append(&sock->connl, &conn->le, conn);
	conn->peer = *peer;
	conn->sock = sock;

	err = tcp_accept(&conn->tc, sock->ts, NULL, recv_handler,
		close_handler, conn);
	if (err)
		goto out;

#ifdef USE_TLS
	if (sock->tls) {
		err = tls_start_tcp(&conn->sc, sock->tls, conn->tc, 0);
		if (err)
			goto out;
	}
#endif

	tmr_start(&conn->tmr, TIMEOUT_INIT, timeout_handler, conn);

 out:
	if (err) {
		mem_deref(conn);
		tcp_reject(sock->ts);
	}
}


/**
 * create an RTSP socket
 *
 * @param sockp			Pointer to returned RTSP Socket
 * @param laddr			Network addess to listen on
 * @param sockmsgh	Message Handler
 * @param arg				Handler argument
 *
 * @return 					0 if success, otherwise errorcode
 */
int rtsp_listen(struct rtsp_sock **sockp, const struct sa *laddr,
		rtsp_sock_msg_h *sockmsgh, void *arg)
{
	struct rtsp_sock *sock;
	int err;

	if (!sockp || !laddr || !sockmsgh)
		return EINVAL;

	sock = mem_zalloc(sizeof(*sock), sock_destructor);
	if (!sock)
		return ENOMEM;

	err = tcp_listen(&sock->ts, laddr, connect_handler, sock);
	if (err)
		goto out;

	sock->sockmsgh = sockmsgh;
	sock->arg = arg;

 out:
	if (err)
		mem_deref(sock);
	else
		*sockp = sock;

	return err;
}


/**
 * create Secure RTSP socket (RTSP V2.0 only)
 *
 * @param sockp			Pointer to returned RTSP Socket
 * @param laddr			Network addess to listen on
 * @param cert			Rile path of TLS certificate
 * @param sockmsgh	Message Handler
 * @param arg				Handler argument
 *
 * @return 					0 if success, otherwise errorcode
 */
int rtsps_listen(struct rtsp_sock **sockp, const struct sa *laddr,
		const char *cert, rtsp_sock_msg_h *sockmsgh, void *arg)
{
	struct rtsp_sock *sock;
	int err;

	if (!sockp || !laddr || !cert || !sockmsgh)
		return EINVAL;

	err = rtsp_listen(&sock, laddr, sockmsgh, arg);
	if (err)
		return err;

#ifdef USE_TLS
	err = tls_alloc(&sock->tls, TLS_METHOD_SSLV23, cert, NULL);
#else
	err = EPROTONOSUPPORT;
#endif
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(sock);
	else
		*sockp = sock;

	return err;
}


struct tcp_sock *rtsp_sock_tcp(struct rtsp_sock *sock)
{
	return sock ? sock->ts : NULL;
}


const struct sa *rtsp_conn_peer(const struct rtsp_conn *conn)
{
	return conn ? &conn->peer : NULL;
}


struct tcp_conn *rtsp_conn_tcp(struct rtsp_conn *conn)
{
	return conn ? conn->tc : NULL;
}


struct tls_conn *rtsp_conn_tls(struct rtsp_conn *conn)
{
	return conn ? conn->sc : NULL;
}


void rtsp_conn_close(struct rtsp_conn *conn)
{
	if (!conn)
		return;

	conn->sc = mem_deref(conn->sc);
	conn->tc = mem_deref(conn->tc);
}


/* WRITE FUNCTIONS */
static int rtsp_vreply(const struct rtsp_conn *conn, uint8_t ver,
	uint16_t scode, const char *reason, const char *fmt, va_list ap)
{
	struct mbuf *mb;
	int err;

	if (!conn || !ver || !scode || !reason)
		return EINVAL;

	if (!conn->tc)
		return ENOTCONN;

	mb = mbuf_alloc(8192);
	if (!mb)
		return ENOMEM;

	err = mbuf_printf(mb, "RTSP/%u.0 %u %s\r\n", ver, scode, reason);
	if (fmt)
		err |= mbuf_vprintf(mb, fmt, ap);
	else
		err |= mbuf_write_str(mb, "Content-Length: 0\r\n\r\n");

	if (err)
		goto out;

	mb->pos = 0;
	err = tcp_send(conn->tc, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


/**
 * Send an RTSP response
 *
 * @param conn		RTSP connection
 * @param ver			RTSP version (1 / 2)
 * @param scode		Rsponse status code
 * @param reason	Response reason phrase
 * @param fmt			Fromatted RTSP message
 *
 * @return 				0 if success, otherwise errorcode
 */
int rtsp_reply(const struct rtsp_conn *conn, uint8_t ver, uint16_t scode,
	const char *reason, const char *fmt, ...)
{
	va_list ap;
	int err;

	va_start(ap, fmt);
	err = rtsp_vreply(conn, ver, scode, reason, fmt, ap);
	va_end(ap);

	return err;
}


/**
 * Send RTSP respones with with content
 *
 * @param conn		RTSP connection
 * @param ver 		RTSP version (1 / 2)
 * @param scode 	Response status code
 * @param reason	Response reason phrase
 * @param ctype		Body content type
 * @param data		Body data
 * @param fmt			Formated RTSP message
 *
 * @return 				0 if success, otherwise errorcode
 */
int rtsp_creply(const struct rtsp_conn *conn, uint8_t ver, uint16_t scode,
	const char *reason, const char *ctype, struct mbuf *data,
	const char *fmt, ...)
{
	struct mbuf *mb;
	va_list ap;
	int err;

	if (!ctype || !fmt)
		return EINVAL;

	mb = mbuf_alloc(8192);
	if (!mb)
		return ENOMEM;

	va_start(ap, fmt);
	err = mbuf_vprintf(mb, fmt, ap);
	va_end(ap);
	if (err)
		goto out;

	err = rtsp_reply(conn, ver, scode, reason,
		"%b"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n"
		"%b",
		mb->buf, mb->end,
		ctype,
		data->end,
		data->buf, data->end);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


/**
 * Send RTSP Interleaved Data (ILD) Package
 *
 * @param conn		RTSP connection
 * @param ch			ILD channel
 * @param data		ILD data
 * @param n				size of ILD
 *
 * @return				0 if success, otherwise errorcode
 */
int rtsp_send_ild(struct rtsp_conn *conn, uint8_t ch,	uint8_t *data, size_t n)
{
	struct mbuf *mb;
	int err;

	if (!conn || !data)
		return EINVAL;

	if (!conn->tc)
		return ENOTCONN;

	mb = mbuf_alloc(n + 4);
	if (!mb)
		return ENOMEM;

	err = mbuf_write_u8(mb, 0x24);
	err |= mbuf_write_u8(mb, ch);
	err |= mbuf_write_u16(mb, htons(n));
	err |= mbuf_write_mem(mb, data, n);
	if (err)
		goto out;

	mb->pos = 0;
	err = tcp_send(conn->tc, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


static int rtsp_send_vreq(struct rtsp_msg **msgp, struct rtsp_conn *conn,
	uint8_t ver, const char *method, const char *path, const char *fmt,
	va_list ap)
{
	struct mbuf *mb;
	int err;
	struct rtsp_msg *msg = NULL;

	if (!msgp || !conn || !ver || !method || !path)
		return EINVAL;

	if (!conn->tc)
		return ENOTCONN;

	mb = mbuf_alloc(8192);
	if (!mb)
		return ENOMEM;

	err = mbuf_printf(mb, "%s %s RTSP/%u.0\r\n", method, path, ver);
	if (fmt) {
		err |= mbuf_vprintf(mb, fmt, ap);
	} else
		err |= mbuf_write_str(mb, "Content-Length: 0\r\n\r\n");

	if (err)
		goto out;

	mb->pos = 0;
	err = rtsp_msg_decode(&msg, mb, true);
	if (err)
		goto out;

	mb->pos = 0;
	err = tcp_send(conn->tc, mb);
	if (err) {
		mem_deref(msg);
		goto out;
	}

	*msgp = msg;

 out:
	mem_deref(mb);

	return err;
}


/**
 * Send an RTSP request
 *
 * @param msgq			RTSP message pointer
 * @param conn 			RTSP connection
 * @param ver				RTSP version (1 / 2)
 * @param method		RTSP request method
 * @param paht 			Requested path
 * @param fmt				Formatted RTSP message
 *
 * @return					0 if success, otherwise errorcode
 */
int rtsp_send_req(struct rtsp_msg **msgq, struct rtsp_conn *conn, uint8_t ver,
	const char *method, const char *path, const char *fmt, ...)
{
	va_list ap;
	int err;

	va_start(ap, fmt);
	err = rtsp_send_vreq(msgq, conn, ver, method, path, fmt, ap);
	va_end(ap);

	return err;
}


/**
 * Send RTSP respones with with content
 *
 * @param conn		RTSP connection
 * @param ver 		RTSP version (1 / 2)
 * @param method	RTSP request method
 * @param path		Requested path
 * @param ctype		Body content type
 * @param data		Body data
 * @param fmt			Formated RTSP message
 *
 * @return 				0 if success, otherwise errorcode
 */
int rtsp_send_creq(struct rtsp_msg **msgq, struct rtsp_conn *conn, uint8_t ver,
	const char *method, const char *path, const char *ctype, struct mbuf *data,
	const char *fmt, ...)
{
	int err;
	struct mbuf *mb;
	va_list ap;

	if (!ctype || !fmt)
		return EINVAL;

	mb = mbuf_alloc(8192);
	if (!mb)
		return ENOMEM;

	va_start(ap, fmt);
	err = mbuf_vprintf(mb, fmt, ap);
	va_end(ap);
	if (err)
		goto out;

	err = rtsp_send_req(msgq, conn, ver, method, path,
		"%b"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n"
		"%b",
		mb->buf, mb->end,
		ctype,
		data->end,
		data->buf, data->end);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}