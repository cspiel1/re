/**
 * @file http/client.c HTTP Client
 *
 * Copyright (C) 2011 Creytiv.com
 */

#include <string.h>
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_tmr.h>
#include <re_srtp.h>
#include <re_tcp.h>
#include <re_tls.h>
#include <re_dns.h>
#include <re_msg.h>
#include <re_http.h>
#include "http.h"


#define DEBUG_MODULE "client"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

enum {
	CONN_TIMEOUT = 30000,
	RECV_TIMEOUT = 60000,
	IDLE_TIMEOUT = 900000,
	BUFSIZE_MAX  = 524288,
	CONN_BSIZE   = 256,
};

struct http_cli {
	struct http_conf conf;
	struct list reql;
	struct hash *ht_conn;
	struct dnsc *dnsc;
	struct tls *tls;
	char *tls_hostname;
};

struct conn;

struct http_req {
	struct http_chunk chunk;
	struct sa srvv[16];
	struct le le;
	struct http_req **reqp;
	struct http_cli *cli;
	struct http_msg *msg;
	struct dns_query *dq;
	struct conn *conn;
	struct mbuf *mbreq;
	struct mbuf *mb;
	char *host;
	http_resp_h *resph;
	http_data_h *datah;
	http_conn_h *connh;
	void *arg;
	size_t rx_len;
	unsigned srvc;
	uint16_t port;
	bool chunked;
	bool secure;
	bool close;
};


static const struct http_conf default_conf = {
	CONN_TIMEOUT,
	RECV_TIMEOUT,
	IDLE_TIMEOUT,
};


struct conn {
	struct tmr tmr;
	struct sa addr;
	struct le he;
	struct http_req *req;
	struct tls_conn *sc;
	struct tcp_conn *tc;
	uint64_t usec;
};


static void req_close(struct http_req *req, int err,
		      const struct http_msg *msg);
static int req_connect(struct http_req *req);
static void timeout_handler(void *arg);


static void cli_destructor(void *arg)
{
	struct http_cli *cli = arg;
	struct le *le = cli->reql.head;

	while (le) {
		struct http_req *req = le->data;

		le = le->next;
		req_close(req, ECONNABORTED, NULL);
	}

	hash_flush(cli->ht_conn);
	mem_deref(cli->ht_conn);
	mem_deref(cli->dnsc);
	mem_deref(cli->tls);
	mem_deref(cli->tls_hostname);
}


static void req_destructor(void *arg)
{
	struct http_req *req = arg;

	list_unlink(&req->le);
	mem_deref(req->msg);
	mem_deref(req->dq);
	mem_deref(req->conn);
	mem_deref(req->mbreq);
	mem_deref(req->mb);
	mem_deref(req->host);
}


static void conn_destructor(void *arg)
{
	struct conn *conn = arg;

	tmr_cancel(&conn->tmr);
	hash_unlink(&conn->he);
	mem_deref(conn->sc);
	mem_deref(conn->tc);
}


static void conn_idle(struct conn *conn)
{
	struct http_req *req;
	struct http_cli *cli;
	if (!conn)
		return;

	req =  conn->req;
	if (req)
		return;

	cli = req->cli;
	if (cli)
		return;

	tmr_start(&conn->tmr, cli->conf.idle_timeout, timeout_handler, conn);
	conn->req = NULL;
}


static void req_close(struct http_req *req, int err,
		      const struct http_msg *msg)
{
	list_unlink(&req->le);
	req->dq = mem_deref(req->dq);
	req->datah = NULL;

	if (req->conn) {
		if (req->connh)
			req->connh(req->conn->tc, req->conn->sc, req->arg);

		if (err || req->close || req->connh)
			mem_deref(req->conn);
		else
			conn_idle(req->conn);

		req->conn = NULL;
	}

	req->connh = NULL;

	if (req->reqp) {
		*req->reqp = NULL;
		req->reqp = NULL;
	}

	if (req->resph) {
		if (msg)
			msg->mb->pos = 0;

		req->resph(err, msg, req->arg);
		req->resph = NULL;
	}

	mem_deref(req);
}


static void try_next(struct conn *conn, int err)
{
	struct http_req *req = conn->req;
	bool retry = conn->usec > 1;

	mem_deref(conn);

	if (!req)
		return;

	req->conn = NULL;

	if (retry)
		++req->srvc;

	if (req->srvc > 0 && !req->msg) {

		err = req_connect(req);
		if (!err)
			return;
	}

	req_close(req, err, NULL);
}


static int write_body_buf(struct http_msg *msg, const uint8_t *buf, size_t sz)
{
	if ((msg->mb->pos + sz) > BUFSIZE_MAX)
		return EOVERFLOW;

	return mbuf_write_mem(msg->mb, buf, sz);
}


static int write_body(struct http_req *req, struct mbuf *mb)
{
	const size_t size = min(mbuf_get_left(mb), req->rx_len);
	int err;

	if (size == 0)
		return 0;

	if (req->datah)
		err = req->datah(mbuf_buf(mb), size, req->msg, req->arg);
	else
		err = write_body_buf(req->msg, mbuf_buf(mb), size);

	if (err)
		return err;

	req->rx_len -= size;
	mb->pos     += size;

	return 0;
}


static int req_recv(struct http_req *req, struct mbuf *mb, bool *last)
{
	int err;

	*last = false;

	if (!req->chunked) {

		err = write_body(req, mb);
		if (err)
			return err;

		if (req->rx_len == 0)
			*last = true;

		return 0;
	}

	while (mbuf_get_left(mb)) {

		if (req->rx_len == 0) {

			err = http_chunk_decode(&req->chunk, mb, &req->rx_len);
			if (err == ENODATA)
				return 0;
			else if (err)
				return err;
			else if (req->rx_len == 0) {
				*last = true;
				return 0;
			}
		}

		err = write_body(req, mb);
		if (err)
			return err;
	}

	return 0;
}


static void timeout_handler(void *arg)
{
	struct conn *conn = arg;

	try_next(conn, ETIMEDOUT);
}


static void estab_handler(void *arg)
{
	struct conn *conn = arg;
	struct http_req *req = conn->req;
	struct http_cli *cli;
	int err;

	if (!req)
		return;

	err = tcp_send(conn->tc, req->mbreq);
	if (err) {
		try_next(conn, err);
		return;
	}

	cli = req->cli;
	if (!cli)
		return;

	tmr_start(&conn->tmr, cli->conf.recv_timeout, timeout_handler, conn);
}


static void recv_handler(struct mbuf *mb, void *arg)
{
	const struct http_hdr *hdr;
	struct conn *conn = arg;
	struct http_req *req = conn->req;
	size_t pos;
	bool last;
	int err;

	if (!req)
		return;

	if (req->msg) {
		err = req_recv(req, mb, &last);
		if (err || last)
			goto out;

		return;
	}

	if (req->mb) {

		const size_t len = mbuf_get_left(mb);

		if ((mbuf_get_left(req->mb) + len) > BUFSIZE_MAX) {
			err = EOVERFLOW;
			goto out;
		}

		pos = req->mb->pos;
		req->mb->pos = req->mb->end;

		err = mbuf_write_mem(req->mb, mbuf_buf(mb), len);
		if (err)
			goto out;

		req->mb->pos = pos;
	}
	else {
		req->mb = mem_ref(mb);
	}

	pos = req->mb->pos;

	err = http_msg_decode(&req->msg, req->mb, false);
	if (err) {
		if (err == ENODATA) {
			req->mb->pos = pos;
			return;
		}
		goto out;
	}

	if (req->datah)
		tmr_cancel(&conn->tmr);

	hdr = http_msg_hdr(req->msg, HTTP_HDR_CONNECTION);
	if (hdr && !pl_strcasecmp(&hdr->val, "close"))
		req->close = true;

	if (http_msg_hdr_has_value(req->msg, HTTP_HDR_TRANSFER_ENCODING,
				   "chunked"))
		req->chunked = true;
	else
		req->rx_len = req->msg->clen;

	err = req_recv(req, req->mb, &last);
	if (err || last)
		goto out;

	return;

 out:
	req_close(req, err, req->msg);
}


static void close_handler(int err, void *arg)
{
	struct conn *conn = arg;

	try_next(conn, err ? err : ECONNRESET);
}


static bool conn_cmp(struct le *le, void *arg)
{
	const struct conn *conn = le->data;
	const struct http_req *req = arg;

	if (!sa_cmp(&req->srvv[req->srvc], &conn->addr, SA_ALL))
		return false;

	if (req->secure != !!conn->sc)
		return false;

	return conn->req == NULL;
}


static int conn_connect(struct http_req *req)
{
	const struct sa *addr = &req->srvv[req->srvc];
	struct conn *conn;
	struct http_cli *cli;
	int err;

	conn = list_ledata(hash_lookup(req->cli->ht_conn,
				       sa_hash(addr, SA_ALL), conn_cmp, req));
	if (conn) {
		err = tcp_send(conn->tc, req->mbreq);
		if (!err) {
			cli = req->cli;
			if (!cli)
				return EINVAL;

			tmr_start(&conn->tmr, cli->conf.recv_timeout,
				  timeout_handler, conn);

			req->conn = conn;
			conn->req = req;

			++conn->usec;

			return 0;
		}

		mem_deref(conn);
	}

	conn = mem_zalloc(sizeof(*conn), conn_destructor);
	if (!conn)
		return ENOMEM;

	hash_append(req->cli->ht_conn, sa_hash(addr, SA_ALL), &conn->he, conn);

	conn->addr = *addr;
	conn->usec = 1;

	err = tcp_connect(&conn->tc, addr, estab_handler, recv_handler,
			  close_handler, conn);
	if (err)
		goto out;

#ifdef USE_TLS
	if (req->secure) {

		err = tls_start_tcp(&conn->sc, req->cli->tls, conn->tc, 0);
		if (err)
			goto out;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
		if (req->cli->tls_hostname)
			err = tls_peer_set_verify_host(conn->sc, req->cli->tls_hostname);
#endif

		if (err)
			goto out;

		err = tls_set_servername(conn->sc, req->host);
		if (err)
			goto out;
	}
#endif

	tmr_start(&conn->tmr, req->cli->conf.conn_timeout, timeout_handler, conn);

	req->conn = conn;
	conn->req = req;

 out:
	if (err)
		mem_deref(conn);

	return err;
}


static int req_connect(struct http_req *req)
{
	int err = EINVAL;

	while (req->srvc > 0) {

		--req->srvc;

		req->mb = mem_deref(req->mb);

		err = conn_connect(req);
		if (!err)
			break;
	}

	return err;
}


static bool rr_handler(struct dnsrr *rr, void *arg)
{
	struct http_req *req = arg;

	if (req->srvc >= ARRAY_SIZE(req->srvv))
		return true;

	switch (rr->type) {

	case DNS_TYPE_A:
		sa_set_in(&req->srvv[req->srvc++], rr->rdata.a.addr,
			  req->port);
		break;

	case DNS_TYPE_AAAA:
		sa_set_in6(&req->srvv[req->srvc++], rr->rdata.aaaa.addr,
			   req->port);
		break;
	}

	return false;
}


static void query_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			  struct list *authl, struct list *addl, void *arg)
{
	struct http_req *req = arg;
	(void)hdr;
	(void)authl;
	(void)addl;

	dns_rrlist_apply2(ansl, req->host, DNS_TYPE_A, DNS_TYPE_AAAA,
			  DNS_CLASS_IN, true, rr_handler, req);
	if (req->srvc == 0) {
		err = err ? err : EDESTADDRREQ;
		goto fail;
	}

	err = req_connect(req);
	if (err)
		goto fail;

	return;

 fail:
	req_close(req, err, NULL);
}


int http_uri_decode(struct http_uri *uri, const char* s)
{
	if (!uri)
		return EINVAL;

	memset(uri, 0, sizeof(*uri));

	/* Try IPv6 first */
	if (!re_regex(s, strlen(s), "[a-z]+://\\[[^\\]]+\\][:]*[0-9]*[^]*",
		     &uri->scheme, &uri->host, NULL, &uri->port, &uri->path))
		goto out;

	/* Then non-IPv6 host */
	re_regex(s, strlen(s), "[a-z]+://[^:/]+[:]*[0-9]*[^]*",
		     &uri->scheme, &uri->host, NULL, &uri->port, &uri->path);

out:
	if (!pl_isset(&uri->path))
		pl_set_str(&uri->path, "/");
	return uri->scheme.p != s;
}


/**
 * Send an HTTP request
 *
 * @param reqp      Pointer to allocated HTTP request object
 * @param cli       HTTP Client
 * @param met       Request method
 * @param uri       Request URI
 * @param resph     Response handler
 * @param datah     Content handler (optional)
 * @param arg       Handler argument
 * @param fmt       Formatted HTTP headers and body (optional)
 *
 * @return 0 if success, otherwise errorcode
 */
int http_request(struct http_req **reqp, struct http_cli *cli, const char *met,
		 const char *uri, http_resp_h *resph, http_data_h *datah,
		 void *arg, const char *fmt, ...)
{
	struct http_uri http_uri;
	struct http_req *req;
	uint16_t defport;
	bool secure;
	va_list ap;
	int err;

	if (!cli || !met || !uri)
		return EINVAL;

	if (http_uri_decode(&http_uri, uri))
		return EINVAL;

	if (!pl_strcasecmp(&http_uri.scheme, "http") ||
	    !pl_strcasecmp(&http_uri.scheme, "ws")) {
		secure  = false;
		defport = 80;
	}
#ifdef USE_TLS
	else if (!pl_strcasecmp(&http_uri.scheme, "https") ||
		 !pl_strcasecmp(&http_uri.scheme, "wss")) {
		secure  = true;
		defport = 443;
	}
#endif
	else
		return ENOTSUP;

	req = mem_zalloc(sizeof(*req), req_destructor);
	if (!req)
		return ENOMEM;

	list_append(&cli->reql, &req->le, req);

	req->cli    = cli;
	req->secure = secure;
	req->port   = pl_isset(&http_uri.port) ? pl_u32(&http_uri.port) : defport;
	req->resph  = resph;
	req->datah  = datah;
	req->arg    = arg;

	err = pl_strdup(&req->host, &http_uri.host);
	if (err)
		goto out;

	req->mbreq = mbuf_alloc(1024);
	if (!req->mbreq) {
		err = ENOMEM;
		goto out;
	}

	err = mbuf_printf(req->mbreq,
			  "%s %r HTTP/1.1\r\n"
			  "Host: %r\r\n",
			  met, &http_uri.path, &http_uri.host);
	if (fmt) {
		va_start(ap, fmt);
		err |= mbuf_vprintf(req->mbreq, fmt, ap);
		va_end(ap);
	}
	else {
		err |= mbuf_write_str(req->mbreq, "\r\n");
	}
	if (err)
		goto out;

	req->mbreq->pos = 0;

	if (!sa_set_str(&req->srvv[0], req->host, req->port)) {

		req->srvc = 1;

		err = req_connect(req);
		if (err)
			goto out;
	}
	else {
		err = dnsc_query(&req->dq, cli->dnsc, req->host,
				 DNS_TYPE_A, DNS_CLASS_IN, true,
				 query_handler, req);
		if (err)
			goto out;
	}

 out:
	if (err)
		mem_deref(req);
	else if (reqp) {
		req->reqp = reqp;
		*reqp = req;
	}

	return err;
}


/**
 * Set HTTP request connection handler
 *
 * @param req   HTTP request object
 * @param connh Connection handler
 */
void http_req_set_conn_handler(struct http_req *req, http_conn_h *connh)
{
	if (!req)
		return;

	req->connh = connh;
}


int http_client_set_config(struct http_cli *clip, struct http_conf *conf)
{
	if (!clip || !conf)
		return EINVAL;

	clip->conf = *conf;
	return 0;
}


/**
 * Allocate an HTTP client instance
 *
 * @param clip      Pointer to allocated HTTP client
 * @param dnsc      DNS Client
 *
 * @return 0 if success, otherwise errorcode
 */
int http_client_alloc(struct http_cli **clip, struct dnsc *dnsc)
{
	struct http_cli *cli;
	int err;

	if (!clip || !dnsc)
		return EINVAL;

	cli = mem_zalloc(sizeof(*cli), cli_destructor);
	if (!cli)
		return ENOMEM;

	err = hash_alloc(&cli->ht_conn, CONN_BSIZE);
	if (err)
		goto out;

#ifdef USE_TLS
	err = tls_alloc(&cli->tls, TLS_METHOD_SSLV23, NULL, NULL);
	if (err)
		goto out;

	err = tls_set_verify_purpose(cli->tls, "sslserver");
	if (err)
		goto out;
#else
	err = 0;
#endif

	cli->dnsc = mem_ref(dnsc);
	cli->conf = default_conf;

 out:
	if (err)
		mem_deref(cli);
	else
		*clip = cli;

	return err;
}


#ifdef USE_TLS
/**
 * Add trusted CA certificates
 *
 * @param cli     HTTP client
 * @param capath  Path to CA certificates
 *
 * @return 0 if success, otherwise errorcode
 */
int http_client_add_ca(struct http_cli *cli, const char *tls_ca)
{
	if (!cli || !tls_ca)
		return EINVAL;

	return tls_add_ca(cli->tls, tls_ca);
}


/**
 * Add trusted CA certificates given as string.
 * @param cli    HTTP client
 * @param capem  The trusted CA as null-terminated string given in PEM format.
 *
 * @return 0 for success, error code otherwise.
 */
/* ------------------------------------------------------------------------- */
int http_client_add_capem(struct http_cli *cli, const char *capem)
{
	return tls_add_capem(cli->tls, capem);
}


/**
 * Set client certificate
 * @param cli    HTTP client
 * @param cert   File path to client certificate.
 *
 * @return 0 for success, error code otherwise.
 */
/* ------------------------------------------------------------------------- */
int http_client_set_client_cert(struct http_cli *cli, const char *cert_file_path)
{
	FILE *cert_file = NULL;
	size_t file_size = 0;
	char *cert_data = NULL;
	int res = 0;

	cert_file = fopen(cert_file_path, "r");
	if (!cert_file) {
		res = EINVAL;
		DEBUG_WARNING("Could not open file '%s'\n", cert_file_path);
		goto out;
	}

	fseek(cert_file, 0L, SEEK_END);
	file_size = ftell(cert_file);
	fseek(cert_file, 0L, SEEK_SET);

	cert_data = mem_alloc(file_size + 1, NULL);
	if (!cert_data) {
		res = ENOMEM;
		DEBUG_WARNING("Could not allocate cert file memory\n");
		goto close_file_out;
	}

	fread(cert_data, 1, file_size, cert_file);
	cert_data[file_size] = 0;

	res = tls_set_certificate(cli->tls, cert_data, file_size);

	mem_deref(cert_data);

close_file_out:
	fclose(cert_file);
out:
	return res;
}


/**
 * Set verify host name
 *
 * @param cli       HTTP client
 * @param hostname  String for alternative name validation.
 *
 * @return 0 if success, otherwise errorcode
 */
int http_client_set_tls_hostname(struct http_cli *cli, const struct pl *hostname)
{
	if (!cli || !hostname)
		return EINVAL;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
	DEBUG_WARNING("verify hostname needs openssl version 1.1.0\n");
	return EINVAL;
#endif

	return pl_strdup(&cli->tls_hostname, hostname);
}

/**
 * Get verify host name
 *
 * @param cli       HTTP client
 *
 * @return          String for alternative name validation.
 */
const char *http_client_get_tls_hostname(struct http_cli *cli)
{
	return cli->tls_hostname;
}
#endif

