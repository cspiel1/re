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

#include <string.h>
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
	(void) err;

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


// int urtsp_liste(...)
// {

// }



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


void rtsp_conn_close(struct rtsp_conn *conn)
{
	if (!conn)
		return;

	conn->sc = mem_deref(conn->sc);
	conn->tc = mem_deref(conn->tc);
}

/* WRITE FUNCTIONS */