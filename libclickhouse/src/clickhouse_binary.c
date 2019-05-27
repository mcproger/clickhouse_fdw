#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stropts.h>
#include "clickhouse_net.h"
#include "clickhouse_binary.h"
#include "clickhouse_config.h"

static bool in_error_state = false;
static char last_error[2048];

#ifdef __GNUC__
	 __attribute__ (( format( gnu_printf, 1, 2 ) ))
#endif
void
ch_error(const char *fmt, ...)
{
	in_error_state = true;

	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void
ch_reset_error(void)
{
	in_error_state = false;
}

int
ch_binary_errno(void)
{
	return in_error_state ? 1 : 0;
}

const char *
ch_binary_last_error(void)
{
	if (in_error_state)
		return (const char *) last_error;
	return NULL;
}

static int
ch_connect(struct sockaddr *sa, int timeout)
{
	int addrlen,
		rc,
		sock;

	int on = 1;

	sock = socket(sa->sa_family, SOCK_STREAM, 0);
	if (sock == -1)
		return -1;

	if (sa->sa_family == AF_INET)
		addrlen = sizeof(struct sockaddr_in);
	else if (sa->sa_family == AF_INET6)
		addrlen = sizeof(struct sockaddr_in6);
	else if (sa->sa_family == AF_UNIX)
		addrlen = sizeof(struct sockaddr_un);
	else
		goto fail;

#if defined(O_NONBLOCK)
	fcntl(sock, F_SETFL, O_NONBLOCK);
#else
	int flags;

    /* Otherwise, use the old way of doing it */
    flags = 1;
	ioctl(fd, FIOBIO, &flags);
#endif

	rc = connect(sock, sa, addrlen);
	if (rc == 0)
		goto done;

	int result_errno = errno;
	if (result_errno == EINPROGRESS)
	{
		struct timeval tv = {timeout,0};
		fd_set	rset,
				wset;

		FD_ZERO (&rset);
		FD_SET  (sock, &rset);
		wset = rset;

		rc = select(FD_SETSIZE, &rset, &wset, NULL, &tv);
		if (rc == 0)
		{
			ch_error("connection timed out");
			goto fail;
		} else if (rc < 0)
		{
			ch_error("connection error: %s", strerror(result_errno));
			goto fail;
		}
		// all good
	}
	else
	{
		ch_error("connection error: %s", strerror(result_errno));
		goto fail;
	}
done:
#ifdef TCP_NODELAY
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on, sizeof(on)) < 0)
	{
		ch_error("setsockopt(%s) failed: %m", "TCP_NODELAY");
		goto fail;
	}
#endif

	on = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
				   (char *) &on, sizeof(on)) < 0)
	{
		ch_error("setsockopt(%s) failed: %m", "SO_KEEPALIVE");
		goto fail;
	}

	return sock;
fail:
	if (sock > 0)
		close(sock);

	return -1;
}

static bool
has_control_character(char *s)
{
	for (size_t i = 0; i < strlen(s); i++)
		if (s[i] < 31)
			return true;

	return false;
};

static bool
ch_binary_send(ch_binary_connection_t *conn)
{
	int n;
	int flags = 0;

#ifdef HAVE_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif

	ch_reset_error();
	if (ch_readahead_unread(&conn->out) == 0)
		// nothing to send
		return true;

again:
	n = send(conn->sock,
			ch_readahead_pos_read(&conn->out),
			ch_readahead_unread(&conn->out), flags);

	ch_readahead_pos_read_advance(&conn->out, ch_readahead_unread(&conn->out));

	if (n < 0)
	{
		int result_errno = errno;
		switch (result_errno)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				goto again;

			case EPIPE:
#ifdef ECONNRESET
				/* FALL THRU */
			case ECONNRESET:
#endif
				ch_error("server closed the connection unexpectedly");
				break;

			default:
				ch_error("could not send data to server");
				break;
		}

		return false;
	}

	return true;
}

int
sock_read(ch_readahead_t *readahead)
{
	int		n,
			rc;
	size_t	left = ch_readahead_left(readahead);

	if (!left)
	{
		/* reader should deal with unread data first */
		return ch_readahead_unread(readahead);
	}

	if (readahead->sock == 0)
		return 0;

again:
	n = recv(readahead->sock, ch_readahead_pos(readahead), left, 0);
	if (n >= 0)
	{
		ch_readahead_pos_advance(readahead, n);
		return n;
	}

	int result_errno = errno;
	switch (result_errno)
	{
		case EAGAIN:
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		case EWOULDBLOCK:
#endif
		{
			fd_set	rset;

			FD_ZERO (&rset);
			FD_SET  (readahead->sock, &rset);

			rc = select(FD_SETSIZE, &rset, NULL, NULL, readahead->timeout);
			if (rc > 0)
				goto again;

			if (rc == 0)
				ch_error("recv timed out");
			else
				ch_error("recv error: %s", strerror(errno));

			return -1;
		}
		case EINTR:
			goto again;

#ifdef ECONNRESET
			/* FALL THRU */
		case ECONNRESET:
			ch_error("server closed the connection unexpectedly");
			break;
#endif

		default:
			ch_error("could not send data to server");
			break;
	}

	return -1;
}

static int
ch_binary_read_header(ch_binary_connection_t *conn)
{
	uint64_t	val;

	ch_readahead_reuse(&conn->in);
	if (ch_readahead_unread(&conn->in) == 0)
	{
		if (sock_read(&conn->in) <= 0)
			return -1;
	}

	if (ch_readahead_unread(&conn->in) == 0)
	{
		ch_error("server communication error");
		return -1;
	}

	val = read_varuint_binary(&conn->in);
	if (val >= CH_MaxPacketType)
	{
		ch_error("imcompatible server, invalid packet type");
		return -1;
	}

	return (int) val;
}

/* send hello packet */
static bool
say_hello(ch_binary_connection_t *conn)
{
	ch_reset_error();
    if (has_control_character(conn->default_database)
        || has_control_character(conn->user)
        || has_control_character(conn->password)
        || has_control_character(conn->client_name))
	{
        ch_error("Parameters 'default_database', 'user' and 'password' must not contain ASCII control characters");
		return false;
	}

	ch_readahead_reuse(&conn->in);
	assert(conn->out.pos == 0);

    write_varuint_binary(&conn->out, CH_Client_Hello);
    write_string_binary(&conn->out, conn->client_name);
    write_varuint_binary(&conn->out, VERSION_MAJOR);
    write_varuint_binary(&conn->out, VERSION_MINOR);
    write_varuint_binary(&conn->out, VERSION_REVISION);
    write_string_binary(&conn->out, conn->default_database);
    write_string_binary(&conn->out, conn->user);
    write_string_binary(&conn->out, conn->password);

	bool res = ch_binary_send(conn);
	return res;
}

/* read hello packet */
static bool
get_hello(ch_binary_connection_t *conn)
{
	int packet_type;

	ch_reset_error();
	packet_type = ch_binary_read_header(conn);
	if (packet_type < 0)
		return false;

    if (packet_type == CH_Hello)
    {
		conn->server_name = read_string_binary(&conn->in);
		if (conn->server_name == NULL)
			return false;

		conn->server_version_major = read_varuint_binary(&conn->in);
		conn->server_version_minor = read_varuint_binary(&conn->in);
		conn->server_revision = read_varuint_binary(&conn->in);

        if (conn->server_revision >= DBMS_MIN_REVISION_WITH_SERVER_TIMEZONE)
		{
            conn->server_timezone = read_string_binary(&conn->in);
			if (conn->server_timezone == NULL)
				return false;
		}
        if (conn->server_revision >= DBMS_MIN_REVISION_WITH_SERVER_DISPLAY_NAME)
		{
            conn->server_display_name = read_string_binary(&conn->in);
			if (conn->server_display_name == NULL)
				return false;
		}
        if (conn->server_revision >= DBMS_MIN_REVISION_WITH_VERSION_PATCH)
			conn->server_version_patch = read_varuint_binary(&conn->in);
        else
            conn->server_version_patch = conn->server_revision;
    }
    else
    {
		ch_error("wrong packet on hello: %d", packet_type);
		return false;
    }

	if (ch_binary_errno() > 0)
		// something happened in between
		return false;

	return true;
}

bool
ch_ping(ch_binary_connection_t *conn)
{
	int packet_type;

	ch_reset_error();
	ch_readahead_reuse(&conn->out);
	assert(conn->out.pos == 0);

    write_varuint_binary(&conn->out, CH_Client_Ping);
	bool res = ch_binary_send(conn);
	if (!res)
		return false;

again:
	packet_type = ch_binary_read_header(conn);
	switch (packet_type)
	{
		case CH_Progress:
			/* TODO: late progress packet, process it */
			goto again;
		case CH_Pong:
			return true;
		default:
			break;
	}

	return false;
}

static ch_binary_client_info_t *
get_default_client_info(void)
{
	static ch_binary_client_info_t *client_info = malloc(sizeof(ch_binary_client_info_t));

	client_infofillOSUserHostNameAndVersionInfo();

	rc = gethostname(client_info->hostname, sizeof(client_info->hostname));
	if (rc == 0)
	{
		ch_error("could not get hostname: %s", strerror(errno));
		goto fail;
	}

    rc = getlogin_r(client_info->os_user, sizeof(client_info->os_user));
	if (rc == 0)
		/* it's ok if can't get username */
		client_info->os_user[0] = '\0';

	client_info->version_major = DBMS_VERSION_MAJOR;
	client_info->version_minor = DBMS_VERSION_MINOR;
	client_info->version_patch = DBMS_VERSION_PATCH;
	client_info->version_revision = DBMS_VERSION_REVISION;

	return client_info;

fail:
	free(client_info);
	client_info = NULL;
	return NULL;
}

static int
write_client_info(ch_binary_connection_t *conn, char *query_id)
{
	char query_kind;
	ch_binary_client_info_t	*client_info;

	query_kind = CH_KIND_INITIAL_QUERY;
	client_info = get_default_client_info(client_name);
	if (client_info == NULL)
		return -1;

    write_char_binary(&conn->out, query_kind);
    write_string_binary(&conn->out, conn->user);
    write_string_binary(&conn->out, query_id);
    write_string_binary(&conn->out, conn->address_str);
    write_char_binary(&conn->out, 1);	// tcp
	write_string_binary(&conn->out, client_info->os_user);
	write_string_binary(&conn->out, client_info->hostname);
	write_string_binary(&conn->out, client_name);
	write_varuint_binary(&conn->out, client_info->version_major);
	write_varuint_binary(&conn->out, client_info->version_minor);
	write_varuint_binary(&conn->out, client_info->version_revision);

    if (server_protocol_revision >= DBMS_MIN_REVISION_WITH_QUOTA_KEY_IN_CLIENT_INFO)
        write_string_binary(&conn->out, "");

	if (server_protocol_revision >= DBMS_MIN_REVISION_WITH_VERSION_PATCH)
		write_varuint_binary(&conn->out, client_info->version_patch);
}

int
ch_binary_send_query(
	ch_binary_connection_t *conn,
	const char *query,
	const char *query_id,
	uint64_t stage,
	ch_binary_query_settings_t *settings)
{
	ch_reset_error();
	ch_readahead_reuse(&conn->out);

    write_varuint_binary(&conn->out, CH_Client_Query);

	/* set default query as empty */
	if (!query_id)
		query_id = "";

	write_string_binary(&conn->out, query_id);

	assert(conn->server_revision);
    if (conn->server_revision >= DBMS_MIN_REVISION_WITH_CLIENT_INFO)
		if (write_client_info(&conn, query_id) != 0)
			return -1;

    /// Per query settings.
    if (settings)
		/* TODO: per query settings */;
    else
        write_string_binary(&conn->out, "");

	write_varuint_binary(&conn->out, stage);
	write_bool_binary(&conn->out, conn->compression > 0);

	assert(query);
    write_string_binary(&conn->out, query);

    /// Send empty block which means end of data.
    if (!with_pending_data)
    {
        sendData(Block());
        out->next();
    }
}

ch_binary_connection_t *
ch_binary_connect(char *host, uint16_t port, char *default_database,
		char *user, char *password, char *client_name, int connection_timeout)
{
	ch_binary_connection_t	*conn;

	char  *address_str = NULL;
	struct addrinfo *ai = NULL;
	struct sockaddr	*saddr = NULL;

	ch_reset_error();
	if (!host || !port)
	{
		ch_error("host or port wasn't specified");
		return NULL;
	}

	int rc_resolve = -1;
	if (strchr(host, ':'))
	{
		struct sockaddr_in6 *saddr_v6 = calloc(sizeof(struct sockaddr_in6), 1);

		/* v6 */
		saddr_v6->sin6_family = AF_INET6;
		saddr_v6->sin6_port   = htons(port);
		rc_resolve = inet_pton(AF_INET6, host, &saddr_v6->sin6_addr);
		saddr = (struct sockaddr*) saddr_v6;
	}
	else
	{
		struct sockaddr_in *saddr_v4 = calloc(sizeof(struct sockaddr_in), 1);

		/* v4 or hostname */
		saddr_v4->sin_family = AF_INET;
		saddr_v4->sin_port   = htons(port);
		rc_resolve = inet_pton(AF_INET, host, &saddr_v4->sin_addr);
		saddr = (struct sockaddr*) saddr_v4;
	};

	/* if it wasn't proper ip, try getaddrinfo */
	if (rc_resolve != 1)
	{
		int rc;
		char sport[16];
		snprintf(sport, sizeof(sport), "%d", port);

		/* free calloced saddr from before */
		free(saddr);

		rc = getaddrinfo(host, sport, NULL, &ai);
		if (rc != 0)
		{
			ch_error("could not resolve host and port");
			return NULL;
		}

		assert(ai != NULL);
		saddr = ai->ai_addr;
	}

	int sock = ch_connect(saddr, connection_timeout);
	if (sock > 0)
		address_str = get_ipaddress(saddr);

	if (ai)
		freeaddrinfo(ai);

	if (sock == -1)
	{
		ch_error("could not create connection to ClickHouse");
		return NULL;
	}

	conn = calloc(sizeof(ch_binary_connection_t), 1);
	conn->sock = sock;
	conn->host = strdup(host);
	conn->port = port;

	/* set default values if needed */
	user = user ? user : "default";
	default_database  = default_database ? default_database : "default";
	password = password ? password : "";
	client_name = client_name ? client_name : "fdw";

	conn->user = strdup(user);
	conn->password = strdup(password);
	conn->default_database = strdup(default_database);
	conn->client_name = strdup(client_name);

	/* construct address for ch protocol */
	assert(address_str != NULL);
	conn->address_str = address_str;

	/* setup timeouts */
	conn->connection_timeout = connection_timeout;
	conn->recv_timeout.tv_sec = 60;
	conn->send_timeout.tv_sec = 60;
	ch_readahead_init(sock, &conn->in, &conn->recv_timeout);
	ch_readahead_init(sock, &conn->out, &conn->send_timeout);

	/* exchange hello packets and initialize server fields in connection */
	if (say_hello(conn) && get_hello(conn))
		/* all good */
		return conn;

	ch_binary_disconnect(conn);
	return NULL;
}

void ch_binary_configure_connection(
		ch_binary_connection_t *conn,
		ch_binary_settings_t *settings,
		ch_binary_timeouts_t *timeouts)
{
	if (timeouts)
	{
		conn->recv_timeout.tv_sec = timeouts->recv_timeout;
		conn->send_timeout.tv_sec = timeouts->send_timeout;
	}

	if (settings)
		conn->compression = settings->compression;
}

void
ch_binary_reconnect(ch_binary_connection_t *conn)
{
}

void
ch_binary_disconnect(ch_binary_connection_t *conn)
{
	if (conn->sock)
		close(conn->sock);

	free(conn->host);
	free(conn->default_database);
	free(conn->user);
	free(conn->password);
	free(conn->client_name);

	if (conn->server_name)
		free(conn->server_name);
	if (conn->server_timezone)
		free(conn->server_timezone);

	free(conn);
}
