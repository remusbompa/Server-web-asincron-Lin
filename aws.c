#include "aws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <sys/sendfile.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"
#include <libaio.h>
#include <sys/eventfd.h>

#include <errno.h>

#define NR_EVENTS 1
/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_PARTIAL_RECEIVED,
	STATE_DATA_SENT,
	STATE_DATA_PARTIAL_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	size_t bytes_sent;
	int fd;
	size_t size;
	int stare;
	int caz;
	off_t offset;

	size_t nr_got;
	size_t nr_trimise;
	size_t nr_sub;

	size_t nr_total;

	size_t last_bytes;
	void **send_buffers;
	enum connection_state state;

	io_context_t ctx;
	int efd;

	struct iocb **piocb;
	struct iocb *iocb;
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	memset(&conn->ctx, 0, sizeof(io_context_t));
	int rs = io_setup(NR_EVENTS, &conn->ctx);
	DIE(rs < 0, strerror(errno));

	conn->sockfd = sockfd;
	conn->stare = 0;
	conn->caz = 0;
	conn->offset = 0;
	conn->recv_len = 0;
	conn->send_len = 0;
	conn->bytes_sent = 0;
	conn->nr_got = 0;
	conn->nr_sub = 0;
	conn->nr_total = 0;
	conn->nr_trimise = 0;
	conn->last_bytes = 0;

	conn->efd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->efd < 0, "error efd");
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);

	return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_sent = send(conn->sockfd, conn->send_buffer + conn->bytes_sent, conn->send_len - conn->bytes_sent, 0);
	if (bytes_sent < 0)
		goto remove_connection;

	conn->bytes_sent += bytes_sent;
	if(conn->bytes_sent < conn->send_len)
		return STATE_DATA_PARTIAL_SENT;

	if (bytes_sent == 0)
		goto remove_connection;

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	close(conn->fd);
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

static http_parser request_parser;
static char request_path[BUFSIZ];	/* storage for request_path */
/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	char path[BUFSIZ];
	sscanf(buf, "%[^.]", path);
	sprintf(request_path, "%s%s.dat",AWS_DOCUMENT_ROOT, path+1);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Receive (HTTP) request. Don't parse it, just read data in buffer
 * and print it.
 */

static enum connection_state receive_request(struct connection *conn)
{
	ssize_t bytes_recv;
	char abuffer[64];
	int rc;

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ - conn->recv_len, 0);
	if (bytes_recv < 0) {		/* error in communication */
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		goto remove_connection;
	}

	conn->recv_len += bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	conn->recv_buffer[conn->recv_len] = 0;
	if(strcmp(conn->recv_buffer + conn->recv_len - 4, "\r\n\r\n") != 0)
		return STATE_DATA_PARTIAL_RECEIVED;

	

	size_t bytes_parsed;

	/* init HTTP_REQUEST parser */
	http_parser_init(&request_parser, HTTP_REQUEST);

	bytes_parsed = http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	if (bytes_parsed == 0)
		goto remove_connection;
	return STATE_DATA_RECEIVED;

remove_connection:
	/* close local socket */
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");
	close(conn->sockfd);
	rc = io_destroy(conn->ctx);
  DIE(rc < 0, "io_destroy");

	/* remove current connection */
	connection_remove(conn);
	return STATE_CONNECTION_CLOSED;
}

/*
 * Send HTTP reply. Send simple message, don't care about request content.
 *
 * Socket is closed after HTTP reply.
 */

static void put_header(struct connection *conn)
{
	char buffer[BUFSIZ];

	 sprintf(buffer, "HTTP/1.1 200 OK\r\n"
		"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
		"Server: Apache/2.2.9\r\n"
		"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
		"Accept-Ranges: bytes\r\n"
		"Content-Length: %ld\r\n"
		"Vary: Accept-Encoding\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n"
		"\r\n", conn->size);
	conn->send_len = strlen(buffer);
	memcpy(conn->send_buffer, buffer, strlen(buffer));
}

static void put_error(struct connection *conn)
{
	char buffer[BUFSIZ] = "HTTP/1.1 404 Not Found\r\n"
		"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
		"Server: Apache/2.2.9\r\n"
		"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
		"Accept-Ranges: bytes\r\n"
		"Content-Length: 153\r\n"
		"Vary: Accept-Encoding\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n"
		"\r\n";
	conn->send_len = strlen(buffer);
	memcpy(conn->send_buffer, buffer, strlen(buffer));
}


/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	//connection_copy_buffers(conn);
	ret_state = receive_request(conn);
	//if(conn->stopped) return;

	if(ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_RECEIVED)
		return;

	char static_prefix[BUFSIZ];
	char dynamic_prefix[BUFSIZ];

	sprintf(static_prefix, "%sstatic/", AWS_DOCUMENT_ROOT);
	sprintf(dynamic_prefix, "%sdynamic/", AWS_DOCUMENT_ROOT);

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);//inout
	DIE(rc < 0, "w_epoll_add_ptr_out");

	struct stat stat_buf;
	/* Open the input file. */
	conn->fd = open (request_path, O_RDONLY);
	if(conn->fd == -1) {
		conn->caz = 0;
		put_error(conn);
		return;
	}
	/* Stat the input file to obtain its size. */
	fstat (conn->fd, &stat_buf);
	conn->size = stat_buf.st_size;
	conn->offset = 0;

	if (strncmp(request_path, static_prefix, strlen(static_prefix)) == 0) {
		conn->caz = 1;
		conn->stare = 0;
		put_header(conn);
	} else if (strncmp(request_path, dynamic_prefix, strlen(dynamic_prefix)) == 0) {
		conn->caz = 2;
		put_header(conn);
		conn->stare = 0;
	} else {
		put_error(conn);
		conn->caz = 0;
		return ;
	}
}

static void send_file_aio(struct connection* conn) {
	

	int n = conn->size / BUFSIZ, i;
	int nr_bytes, rc;

	if(conn->size % BUFSIZ) n++;
	
	conn->iocb = malloc(n * sizeof(struct iocb));
	conn->piocb = malloc(n * sizeof(struct iocb *));
	if (!conn->iocb || !conn->piocb) {
		perror("iocb alloc");
		return;
	}

	conn->send_buffers = malloc(n * sizeof(char*));


	for (i = 0; i < n; i++) {
		conn->send_buffers[i] = malloc(BUFSIZ * sizeof(char));
		conn->piocb[i] = &conn->iocb[i];
		if(conn->size - conn->offset <= BUFSIZ)
			nr_bytes = conn->size - conn->offset;
		else
			nr_bytes = BUFSIZ;
		io_prep_pread(&conn->iocb[i], conn->fd, conn->send_buffers[i], nr_bytes, conn->offset);
		io_set_eventfd(&conn->iocb[i], conn->efd);
		conn->offset += nr_bytes;

		if(i == n-1) conn->last_bytes = nr_bytes;
	}
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_conn");

	
	rc = io_submit(conn->ctx, n - conn->nr_sub, conn->piocb + conn->nr_sub);
	DIE(rc < 0, "io_submit");
	conn->nr_sub += rc;
	

	w_epoll_add_efd(epollfd, conn->efd, conn);

	conn->nr_total = n;
	conn->stare = 2;

	//free(iocb);
	//free(piocb);
}

int main(void)
{
	int rc;
	enum connection_state ret_state;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		struct connection *conn = rev.data.ptr;
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else if (rev.events & EPOLLIN) {
				if(conn->caz == 0 || conn->caz == 1) {
					handle_client_request(rev.data.ptr);
				} else if(conn->caz == 2 && conn->stare == 2) {
					struct io_event events[conn->nr_sub];
					u_int64_t efd_val;
					int rc = read(conn->efd, &efd_val, sizeof(efd_val));
					DIE(rc < 0, "read efd");
					rc = io_getevents(conn->ctx, efd_val, efd_val, events, NULL);
					DIE(rc != efd_val, "io_getevents");
					conn->nr_got += efd_val;

					rc = w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
					DIE(rc < 0, "w_epoll_add_ptr_out");
				}
		} else if (rev.events & EPOLLOUT) {
				if(conn->caz == 0) {
					ret_state = send_message(conn);
					if(ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_SENT)
						continue;
					/* all done - remove out notification */
					int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
					DIE(rc < 0, "w_epoll_remove_ptr");
					rc = io_destroy(conn->ctx);
					DIE(rc < 0, "io_destroy");
					close(conn->sockfd);
					close(conn->fd);
					connection_remove(conn);
				} else if(conn->caz == 1 && conn->stare == 0) {
						ret_state = send_message(conn);
						if(ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_SENT)
							continue;
						conn->stare = 1;
						conn->bytes_sent = 0;
				} else if(conn->caz == 1 && conn->stare == 1 ) {
						int nr_bytes;
						if(conn->size - conn->offset <= BUFSIZ)
							nr_bytes = conn->size - conn->offset;
						else
							nr_bytes = BUFSIZ;
						int nr = sendfile (conn->sockfd, conn->fd, &conn->offset, nr_bytes);
						conn->bytes_sent += nr;
						DIE(nr < 0, "eroare trimitere fisier");
						if(nr == 0) {
							conn->stare = 0;
							conn->bytes_sent = 0;
							int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
							DIE(rc < 0, "w_epoll_remove_ptr");
							rc = io_destroy(conn->ctx);
							DIE(rc < 0, "io_destroy");
							close(conn->sockfd);
							close(conn->fd);
							connection_remove(conn);
						}
				} else if(conn->caz == 2 && conn->stare == 0) {
						ret_state = send_message(conn);
						if(ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_SENT)
							continue;
						conn->stare = 1;
						conn->bytes_sent = 0;
				} else if(conn->caz == 2 && conn->stare == 1) {
						send_file_aio(conn);
				} else if(conn->caz == 2 && conn->stare == 2) {
						if(conn->nr_got > conn->nr_trimise) {
							memcpy(conn->send_buffer, conn->send_buffers[conn->nr_trimise], BUFSIZ);
							if(conn->nr_trimise == conn->nr_sub - 1)
								conn->send_len = conn->last_bytes;
							else
								conn->send_len = BUFSIZ;
				
							ret_state = send_message(conn);
							if(ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_SENT)
								continue;
							conn->nr_trimise++;
							conn->bytes_sent = 0;
						}

						if(conn->nr_trimise == conn->nr_got) {
							int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
							DIE(rc < 0, "w_epoll_remove_ptr");
							if(conn->nr_sub < conn->nr_total) {
								int rc = io_submit(conn->ctx, conn->nr_total - conn->nr_sub, conn->piocb + conn->nr_sub);
								DIE(rc < 0, "io_submit");
								conn->nr_sub += rc;
							}
						}

						if(conn->nr_trimise == conn->nr_total) {
							int i;
							for(i = 0; i < conn->nr_sub; i++)
								free(conn->send_buffers[i]);
							free(conn->send_buffers);

							rc = w_epoll_remove_ptr(epollfd, conn->efd, conn);
							DIE(rc < 0, "w_epoll_remove_efd");
							rc = io_destroy(conn->ctx);
							DIE(rc < 0, "io_destroy");
							close(conn->sockfd);
							close(conn->fd);
							connection_remove(conn);
						}
				}
		}
			
	}

	return 0;
}
