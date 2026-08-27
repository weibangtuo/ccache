// Copyright (C) 2026 weibangtuo and other contributors
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

// Minimal Redis (RESP2 protocol) client used for remote storage of cache
// entries. The configuration syntax and behavior follow the Redis storage
// backend in ccache 4.x, but note that the on-the-wire key and value formats
// are 3.x specific and not interchangeable with 4.x clients:
//
// - Key: "ccache:" + <hash>-<size> (one key for the result entry and one for
//   the manifest; 4.x uses a different hash without the size suffix).
// - Value: the raw bytes of the cache entry (a 3.x bundle, see
//   redis_bundle_encode) or the manifest file (4.x uses its own cache entry
//   format).
// - No TTL: cleanup is expected to be done on the Redis server side, e.g. by
//   configuring LRU eviction.
//
// The implementation intentionally avoids a dependency on hiredis to keep
// building ccache on old platforms (e.g. AIX 5.1) as simple as possible. Only
// plain POSIX socket APIs are used.

#include "ccache.h"
#include "redis.h"

#include <stdint.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#define REDIS_DEFAULT_PORT 6379

// How long to wait for the connection to the Redis server to be established.
// (ccache 4.x defaults to 100 ms with a "connect-timeout" attribute to change
// it; this implementation has no attributes, so use a slightly more lenient
// default.)
#define REDIS_CONNECT_TIMEOUT_MS 2000

// How long to wait for a reply to a Redis command. (Same as ccache 4.x's
// default "operation-timeout".)
#define REDIS_OPERATION_TIMEOUT_MS 10000

#define REDIS_READ_BUFFER_SIZE 65536

// Upper bounds to protect against bogus replies from a misbehaving or
// malicious server: a larger bulk reply (or an unterminated reply line) would
// otherwise make ccache allocate unbounded amounts of memory.
#define REDIS_MAX_BULK_SIZE (128 * 1024 * 1024)
#define REDIS_MAX_LINE_LENGTH 65536

// Header of an encoded cache entry bundle: magic (4 bytes) + version (1 byte)
// + number of files (4 bytes, big endian), followed by per file: file kind
// (1 byte) + file size (8 bytes, big endian) + file data.
#define REDIS_BUNDLE_MAGIC "cCE1"
#define REDIS_BUNDLE_VERSION 1

// ----------------------------------------------------------------------------
// URL parsing (platform independent)

// Percent-decode s[0..len). Returns a NUL-terminated string that the caller
// frees, or NULL if the input is invalid (an encoded NUL byte would silently
// truncate strings passed to strlen-based interfaces). Invalid escape
// sequences are copied verbatim.
static char *
percent_decode(const char *s, size_t len)
{
	char *result = x_malloc(len + 1);
	size_t j = 0;
	for (size_t i = 0; i < len; ++i) {
		if (s[i] == '%' && i + 2 < len && isxdigit((unsigned char)s[i + 1])
		    && isxdigit((unsigned char)s[i + 2])) {
			char hex[3] = {s[i + 1], s[i + 2], '\0'};
			char decoded = (char)strtoul(hex, NULL, 16);
			if (decoded == '\0') {
				free(result);
				return NULL;
			}
			result[j++] = decoded;
			i += 2;
		} else {
			result[j++] = s[i];
		}
	}
	result[j] = '\0';
	return result;
}

// Split percent-decoded userinfo into user and password parts following the
// same semantics as ccache 4.x: "user:password", ":password", "password" or
// "". Takes ownership of userinfo; on return *user and *password (if non-NULL)
// are strings that the caller (via redis_free_url) can free individually.
static void
split_userinfo(char *userinfo, char **user, char **password)
{
	*user = NULL;
	*password = NULL;
	if (!userinfo) {
		return;
	}
	char *colon = strchr(userinfo, ':');
	if (!colon) {
		if (*userinfo != '\0') {
			*password = userinfo;
		} else {
			free(userinfo);
		}
		return;
	}
	if (colon[1] != '\0') {
		*password = x_strdup(colon + 1);
	}
	if (colon != userinfo) {
		*colon = '\0';
		*user = userinfo;
	} else {
		// ":password" or ":" -- no user name.
		free(userinfo);
	}
}

// Parse a plain decimal number in [0, max]. Manual parsing instead of
// strtoul: GCC 4.5 on AIX has a buggy builtin strtoul that doesn't always
// write the endptr, which made the trailing-character check unreliable.
static bool
parse_decimal(const char *s, unsigned long max, unsigned long *result)
{
	unsigned long value = 0;
	if (!s || !*s) {
		return false;
	}
	for (; *s; ++s) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
		unsigned long digit = (unsigned long)(*s - '0');
		if (value > (max - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
	}
	*result = value;
	return true;
}

static bool
parse_db_number(const char *str, unsigned *db, char **errmsg)
{
	if (!str || !*str) {
		*db = 0;
		return true;
	}
	unsigned long value;
	if (!parse_decimal(str, 0xFFFFFFFFUL, &value)) {
		if (errmsg) {
			*errmsg = format("invalid database number \"%s\"", str);
		}
		return false;
	}
	*db = (unsigned)value;
	return true;
}

bool
redis_parse_url(const char *url, struct redis_url *parsed, char **errmsg)
{
	memset(parsed, 0, sizeof(*parsed));
	parsed->port = REDIS_DEFAULT_PORT;

	if (str_startswith(url, "redis+unix:")) {
		parsed->is_unix = true;
		const char *p = url + strlen("redis+unix:");
		char *userinfo = NULL;
		if (str_startswith(p, "//")) {
			p += 2;
			// Authority part: [[username:]password@]localhost
			const char *at = strrchr(p, '@');
			const char *host_start = p;
			if (at) {
				userinfo = percent_decode(p, (size_t)(at - host_start));
				if (!userinfo) {
					if (errmsg) {
						*errmsg = x_strdup("invalid percent-encoding in URL");
					}
					return false;
				}
				host_start = at + 1;
			}
			const char *path_start = strchr(host_start, '/');
			if (!path_start) {
				if (errmsg) {
					*errmsg = x_strdup("missing socket path");
				}
				free(userinfo);
				return false;
			}
			if (path_start != host_start) {
				// Only "localhost" is allowed as host (as in ccache 4.x).
				if (strncmp(host_start, "localhost", 9) != 0
				    || path_start - host_start != 9) {
					if (errmsg) {
						*errmsg = x_strdup(
							"host must be empty or \"localhost\" for redis+unix");
					}
					free(userinfo);
					return false;
				}
			}
			p = path_start;
		}

		const char *query = strchr(p, '?');
		parsed->socket_path = percent_decode(
			p, query ? (size_t)(query - p) : strlen(p));
		if (!parsed->socket_path) {
			if (errmsg) {
				*errmsg = x_strdup("invalid percent-encoding in URL");
			}
			free(userinfo);
			return false;
		}
		if (!str_eq(parsed->socket_path, "")
		    && !is_absolute_path(parsed->socket_path)) {
			if (errmsg) {
				*errmsg = x_strdup("socket path must be absolute");
			}
			free(userinfo);
			redis_free_url(parsed);
			return false;
		}
		if (str_eq(parsed->socket_path, "")) {
			if (errmsg) {
				*errmsg = x_strdup("missing socket path");
			}
			free(userinfo);
			redis_free_url(parsed);
			return false;
		}
		split_userinfo(userinfo, &parsed->user, &parsed->password);
		if (query) {
			if (!str_startswith(query, "?db=")) {
				if (errmsg) {
					*errmsg = x_strdup("unsupported URL query");
				}
				redis_free_url(parsed);
				return false;
			}
			if (!parse_db_number(query + 4, &parsed->db, errmsg)) {
				redis_free_url(parsed);
				return false;
			}
		}
		return true;
	}

	if (str_startswith(url, "redis://")) {
		const char *p = url + strlen("redis://");
		char *userinfo = NULL;
		const char *at = strrchr(p, '@');
		if (at) {
			userinfo = percent_decode(p, (size_t)(at - p));
			if (!userinfo) {
				if (errmsg) {
					*errmsg = x_strdup("invalid percent-encoding in URL");
				}
				return false;
			}
			p = at + 1;
		}

		const char *slash = strchr(p, '/');
		char *hostport = x_strndup(p, slash ? (size_t)(slash - p) : strlen(p));
		char *port_str = strrchr(hostport, ':');
		if (port_str) {
			*port_str++ = '\0';
			unsigned long port;
			if (!parse_decimal(port_str, 65535, &port) || port < 1) {
				if (errmsg) {
					*errmsg = format("invalid port \"%s\"", port_str);
				}
				free(hostport);
				free(userinfo);
				return false;
			}
			parsed->port = (int)port;
		}
		parsed->host = x_strdup(str_eq(hostport, "") ? "localhost" : hostport);
		free(hostport);

		if (!parse_db_number(slash ? slash + 1 : NULL, &parsed->db, errmsg)) {
			free(userinfo);
			redis_free_url(parsed);
			return false;
		}
		split_userinfo(userinfo, &parsed->user, &parsed->password);
		return true;
	}

	if (errmsg) {
		*errmsg = x_strdup(
			"URL must start with \"redis://\" or \"redis+unix:\"");
	}
	return false;
}

void
redis_free_url(struct redis_url *parsed)
{
	free(parsed->host);
	free(parsed->user);
	free(parsed->password);
	free(parsed->socket_path);
	memset(parsed, 0, sizeof(*parsed));
}

char *
redis_url_for_logging(const struct redis_url *url)
{
	if (url->is_unix) {
		return format("redis+unix:%s", url->socket_path);
	} else if (url->db != 0) {
		return format("redis://%s:%d/%u", url->host, url->port, url->db);
	} else {
		return format("redis://%s:%d", url->host, url->port);
	}
}

// ----------------------------------------------------------------------------
// Cache entry bundles (platform independent)

const char *
redis_file_suffix(enum redis_file_kind kind)
{
	switch (kind) {
	case REDIS_FILE_OBJ: return ".o";
	case REDIS_FILE_STDERR: return ".stderr";
	case REDIS_FILE_DEP: return ".d";
	case REDIS_FILE_COV: return ".gcno";
	case REDIS_FILE_SU: return ".su";
	case REDIS_FILE_DIA: return ".dia";
	case REDIS_FILE_DWO: return ".dwo";
	case REDIS_FILE_KIND_COUNT: break;
	}
	return NULL;
}

static void
put_u32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void
put_u64(unsigned char *p, uint64_t v)
{
	for (int i = 0; i < 8; ++i) {
		p[i] = (unsigned char)(v >> (56 - 8 * i));
	}
}

static uint32_t
get_u32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	       | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t
get_u64(const unsigned char *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) {
		v = (v << 8) | p[i];
	}
	return v;
}

// Note: Since x_malloc() aborts on out of memory, this function cannot fail.
void
redis_bundle_encode(const struct redis_bundle *bundle,
                    unsigned char **data, size_t *size)
{
	size_t total = 4 + 1 + 4;
	for (size_t i = 0; i < bundle->n_files; ++i) {
		if (bundle->files[i].size > SIZE_MAX - total - 9) {
			// Overflow guard: only reachable on 32-bit size_t with files
			// adding up to more than the address space (which in practice
			// would have failed allocation earlier). Better to abort with a
			// clear message than to overflow the buffer arithmetic.
			fatal("Cache entry too large for remote storage");
		}
		total += 1 + 8 + bundle->files[i].size;
	}
	unsigned char *buf = x_malloc(total);
	memcpy(buf, REDIS_BUNDLE_MAGIC, 4);
	buf[4] = REDIS_BUNDLE_VERSION;
	put_u32(buf + 5, (uint32_t)bundle->n_files);
	size_t off = 9;
	for (size_t i = 0; i < bundle->n_files; ++i) {
		buf[off++] = (unsigned char)bundle->files[i].kind;
		put_u64(buf + off, bundle->files[i].size);
		off += 8;
		memcpy(buf + off, bundle->files[i].data, bundle->files[i].size);
		off += bundle->files[i].size;
	}
	*data = buf;
	*size = total;
}

struct redis_bundle *
redis_bundle_decode(const unsigned char *data, size_t size)
{
	if (size < 9 || memcmp(data, REDIS_BUNDLE_MAGIC, 4) != 0
	    || data[4] != REDIS_BUNDLE_VERSION) {
		return NULL;
	}
	uint32_t n_files = get_u32(data + 5);
	if (n_files == 0 || n_files > REDIS_FILE_KIND_COUNT) {
		return NULL;
	}

	struct redis_bundle *bundle = x_calloc(1, sizeof(*bundle));
	bundle->files = x_calloc(n_files, sizeof(*bundle->files));
	size_t off = 9;
	for (uint32_t i = 0; i < n_files; ++i) {
		if (off + 9 > size) {
			goto bad_data;
		}
		unsigned kind = data[off++];
		if (kind >= REDIS_FILE_KIND_COUNT) {
			goto bad_data;
		}
		size_t file_size = (size_t)get_u64(data + off);
		off += 8;
		if (file_size > size - off) {
			goto bad_data;
		}
		bundle->files[i].kind = (enum redis_file_kind)kind;
		bundle->files[i].size = file_size;
		bundle->files[i].data = x_malloc(file_size ? file_size : 1);
		memcpy(bundle->files[i].data, data + off, file_size);
		off += file_size;
		// Track allocated files so that the bad_data path frees them.
		bundle->n_files = i + 1;
	}
	if (off != size) {
		goto bad_data;
	}
	bundle->n_files = n_files;
	return bundle;

bad_data:
	redis_bundle_free(bundle);
	return NULL;
}

void
redis_bundle_free(struct redis_bundle *bundle)
{
	if (!bundle) {
		return;
	}
	for (size_t i = 0; i < bundle->n_files; ++i) {
		free(bundle->files[i].data);
	}
	free(bundle->files);
	free(bundle);
}

#ifndef _WIN32

// ----------------------------------------------------------------------------
// RESP2 client

struct redis_connection {
	int fd;
	bool failed;       // set after an I/O or protocol error; no further
	                   // commands are attempted
	unsigned char *buffer;
	size_t buf_size;
	size_t buf_len;    // number of valid bytes in buffer
	size_t buf_pos;    // current read position
};

// Write to the socket without generating SIGPIPE where the platform allows
// it. (The first write error makes the connection fail anyway, but without
// this a write to a reset connection could kill the process with SIGPIPE
// before returning an error on some platforms.)
static ssize_t
socket_write(int fd, const void *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
	return send(fd, buf, len, MSG_NOSIGNAL);
#else
	return write(fd, buf, len);
#endif
}

// Write all data to the socket.
static bool
write_all(struct redis_connection *conn, const void *data, size_t len)
{
	const char *p = data;
	while (len > 0) {
		ssize_t n = socket_write(conn->fd, p, len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			cc_log("Redis: write error: %s", strerror(errno));
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

// Read more data into the buffer, compacting it if needed. Returns false on
// error or EOF.
static bool
fill_buffer(struct redis_connection *conn)
{
	if (conn->buf_pos > 0) {
		memmove(conn->buffer,
		        conn->buffer + conn->buf_pos,
		        conn->buf_len - conn->buf_pos);
		conn->buf_len -= conn->buf_pos;
		conn->buf_pos = 0;
	}
	if (conn->buf_len == conn->buf_size) {
		size_t new_size = conn->buf_size * 2;
		unsigned char *new_buffer = x_realloc(conn->buffer, new_size);
		conn->buffer = new_buffer;
		conn->buf_size = new_size;
	}
	ssize_t n;
	do {
		n = read(conn->fd,
		         conn->buffer + conn->buf_len,
		         conn->buf_size - conn->buf_len);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		cc_log("Redis: read error: %s", strerror(errno));
		return false;
	}
	if (n == 0) {
		cc_log("Redis: connection closed by server");
		return false;
	}
	conn->buf_len += (size_t)n;
	return true;
}

// Make sure that at least n bytes (from the current read position) are
// available in the buffer, growing it if needed.
static bool
buffer_ensure(struct redis_connection *conn, size_t n)
{
	while (conn->buf_len - conn->buf_pos < n) {
		size_t needed = n - (conn->buf_len - conn->buf_pos);
		if (conn->buf_size - conn->buf_len < needed) {
			size_t new_size = conn->buf_size * 2;
			while (new_size - conn->buf_len < needed) {
				if (new_size > SIZE_MAX / 2) {
					// n is unreasonably large (e.g. a bogus bulk length from
					// a corrupt server) and would overflow the size
					// arithmetic.
					cc_log("Redis: reply too large");
					return false;
				}
				new_size *= 2;
			}
			unsigned char *new_buffer = x_realloc(conn->buffer, new_size);
			conn->buffer = new_buffer;
			conn->buf_size = new_size;
		}
		if (!fill_buffer(conn)) {
			return false;
		}
	}
	return true;
}

// Read a CRLF-terminated line. On success *line points into the connection
// buffer (NUL-terminated; valid until the next buffer operation) and *len is
// the line length.
static bool
read_line(struct redis_connection *conn, char **line, size_t *len)
{
	for (;;) {
		unsigned char *start = conn->buffer + conn->buf_pos;
		size_t avail = conn->buf_len - conn->buf_pos;
		unsigned char *nl = memchr(start, '\n', avail);
		if (nl) {
			size_t line_len = (size_t)(nl - start);
			conn->buf_pos += line_len + 1;
			if (line_len > 0 && start[line_len - 1] == '\r') {
				--line_len;
			}
			start[line_len] = '\0';
			*line = (char *)start;
			*len = line_len;
			return true;
		}
		if (avail > REDIS_MAX_LINE_LENGTH) {
			cc_log("Redis: unterminated reply line");
			return false;
		}
		if (!fill_buffer(conn)) {
			return false;
		}
	}
}

// Read n bytes of bulk data plus the trailing CRLF. On success *data points
// into the connection buffer (valid until the next buffer operation).
static bool
read_bytes(struct redis_connection *conn, unsigned char **data, size_t n)
{
	if (!buffer_ensure(conn, n + 2)) {
		return false;
	}
	unsigned char *p = conn->buffer + conn->buf_pos;
	if (p[n] != '\r' || p[n + 1] != '\n') {
		cc_log("Redis: protocol error (missing CRLF after bulk data)");
		return false;
	}
	*data = p;
	conn->buf_pos += n + 2;
	return true;
}

static void
mark_failed(struct redis_connection *conn)
{
	conn->failed = true;
}

// Send a command and read the reply. args is an array of nargs command
// arguments; if value is non-NULL it is sent as an extra binary argument
// (used for SET). If reply is non-NULL and the reply is bulk data, *reply
// (caller frees) and *reply_len receive a copy of the data.
static int
redis_command(struct redis_connection *conn,
              const char *const *args, size_t nargs,
              const unsigned char *value, size_t value_len,
              unsigned char **reply, size_t *reply_len)
{
	if (conn->failed) {
		return REDIS_RESULT_ERROR;
	}

	size_t argc = nargs + (value ? 1 : 0);
	char buf[64];
	// Note: %lu rather than %zu for old libc compatibility (e.g. AIX 5.1).
	snprintf(buf, sizeof(buf), "*%lu\r\n", (unsigned long)argc);
	if (!write_all(conn, buf, strlen(buf))) {
		goto error;
	}
	for (size_t i = 0; i < nargs; ++i) {
		snprintf(buf, sizeof(buf), "$%lu\r\n", (unsigned long)strlen(args[i]));
		if (!write_all(conn, buf, strlen(buf))
		    || !write_all(conn, args[i], strlen(args[i]))
		    || !write_all(conn, "\r\n", 2)) {
			goto error;
		}
	}
	if (value) {
		snprintf(buf, sizeof(buf), "$%lu\r\n", (unsigned long)value_len);
		if (!write_all(conn, buf, strlen(buf))
		    || !write_all(conn, value, value_len)
		    || !write_all(conn, "\r\n", 2)) {
			goto error;
		}
	}

	char *line;
	size_t line_len;
	if (!read_line(conn, &line, &line_len)) {
		goto error;
	}

	switch (line[0]) {
	case '+': // simple string (e.g. +OK)
	case ':': // integer (unused by the commands we issue)
		return REDIS_RESULT_OK;

	case '-': // error
		cc_log("Redis: error reply: %s", line + 1);
		goto error;

	case '$': { // bulk string
		// Manual parsing (see parse_decimal): don't rely on strtol's endptr.
		unsigned long n;
		if (str_eq(line + 1, "-1")) {
			return REDIS_RESULT_MISS;
		}
		if (!parse_decimal(line + 1, REDIS_MAX_BULK_SIZE, &n)) {
			cc_log("Redis: invalid bulk length in reply");
			goto error;
		}
		unsigned char *data;
		if (!read_bytes(conn, &data, (size_t)n)) {
			goto error;
		}
		if (reply) {
			*reply = x_malloc((size_t)n ? (size_t)n : 1);
			memcpy(*reply, data, (size_t)n);
			*reply_len = (size_t)n;
		}
		return REDIS_RESULT_OK;
	}

	default:
		cc_log("Redis: unexpected reply type");
		goto error;
	}

error:
	mark_failed(conn);
	return REDIS_RESULT_ERROR;
}

// Connect a (non-blocking) socket with a timeout and restore blocking mode on
// success.
static bool
connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return false;
	}
	int ret = connect(fd, addr, addrlen);
	if (ret != 0 && errno != EINPROGRESS) {
		return false;
	}
	if (ret != 0) {
		struct pollfd pfd = {fd, POLLOUT, 0};
		int pr;
		do {
			pr = poll(&pfd, 1, REDIS_CONNECT_TIMEOUT_MS);
		} while (pr < 0 && errno == EINTR);
		if (pr <= 0) {
			errno = (pr == 0) ? ETIMEDOUT : errno;
			return false;
		}
		int so_error = 0;
		socklen_t len = sizeof(so_error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0
		    || so_error != 0) {
			if (so_error != 0) {
				errno = so_error;
			}
			return false;
		}
	}
	return fcntl(fd, F_SETFL, flags) == 0;
}

static int
connect_tcp(const struct redis_url *parsed)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		cc_log("Redis: failed to create socket: %s", strerror(errno));
		return -1;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	struct hostent *he = gethostbyname(parsed->host);
	if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]
	    || he->h_length < (int)sizeof(addr.sin_addr)) {
		cc_log("Redis: failed to resolve host \"%s\"", parsed->host);
		close(fd);
		errno = ENOENT;
		return -1;
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)parsed->port);
	memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
	if (!connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		cc_log("Redis: connection failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int
connect_unix(const struct redis_url *parsed)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		cc_log("Redis: failed to create socket: %s", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(parsed->socket_path) >= sizeof(addr.sun_path)) {
		cc_log("Redis: socket path too long");
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(addr.sun_path, parsed->socket_path);
	if (!connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		cc_log("Redis: connection failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

struct redis_connection *
redis_connect(const char *url)
{
	struct redis_url parsed;
	char *errmsg = NULL;
	if (!redis_parse_url(url, &parsed, &errmsg)) {
		cc_log("Redis: invalid remote storage URL: %s", errmsg);
		free(errmsg);
		return NULL;
	}

	char *desc = redis_url_for_logging(&parsed);
	cc_log("Redis: connecting to %s", desc);
	free(desc);

	int fd = parsed.is_unix ? connect_unix(&parsed) : connect_tcp(&parsed);
	if (fd < 0) {
		redis_free_url(&parsed);
		return NULL;
	}

	struct timeval tv;
	tv.tv_sec = REDIS_OPERATION_TIMEOUT_MS / 1000;
	tv.tv_usec = (REDIS_OPERATION_TIMEOUT_MS % 1000) * 1000;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0
	    || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
		// Timeouts are best effort; without them reads/writes may block
		// indefinitely on platforms that don't support the options.
		cc_log("Redis: failed to set socket timeouts: %s", strerror(errno));
	}
	set_cloexec_flag(fd);
#ifdef SO_NOSIGPIPE
	{
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	}
#endif

	struct redis_connection *conn = x_calloc(1, sizeof(*conn));
	conn->fd = fd;
	conn->buf_size = REDIS_READ_BUFFER_SIZE;
	conn->buffer = x_malloc(conn->buf_size);

	bool ok = true;
	if (parsed.password) {
		if (parsed.user) {
			const char *args[] = {"AUTH", parsed.user, parsed.password};
			ok = redis_command(conn, args, 3, NULL, 0, NULL, NULL)
			     == REDIS_RESULT_OK;
		} else {
			const char *args[] = {"AUTH", parsed.password};
			ok = redis_command(conn, args, 2, NULL, 0, NULL, NULL)
			     == REDIS_RESULT_OK;
		}
		if (!ok) {
			cc_log("Redis: authentication failed");
		}
	}
	if (ok && parsed.db != 0) {
		char db_str[16];
		snprintf(db_str, sizeof(db_str), "%u", parsed.db);
		const char *args[] = {"SELECT", db_str};
		ok = redis_command(conn, args, 2, NULL, 0, NULL, NULL)
		     == REDIS_RESULT_OK;
		if (!ok) {
			cc_log("Redis: SELECT %u failed", parsed.db);
		}
	}

	redis_free_url(&parsed);
	if (!ok) {
		redis_disconnect(conn);
		return NULL;
	}

	cc_log("Redis: connection OK");
	return conn;
}

int
redis_get(struct redis_connection *conn, const char *key,
          char **data, size_t *size)
{
	const char *args[] = {"GET", key};
	unsigned char *reply = NULL;
	size_t reply_len = 0;
	int rc = redis_command(conn, args, 2, NULL, 0, &reply, &reply_len);
	if (rc == REDIS_RESULT_OK && !reply) {
		// The server replied with something else than bulk data, e.g. a
		// simple string from a non-Redis service. Don't return an
		// uninitialized pointer to the caller.
		cc_log("Redis: unexpected reply type for GET");
		mark_failed(conn);
		return REDIS_RESULT_ERROR;
	}
	if (rc == REDIS_RESULT_OK) {
		*data = (char *)reply;
		*size = reply_len;
	}
	return rc;
}

int
redis_set(struct redis_connection *conn, const char *key,
          const void *data, size_t size)
{
	const char *args[] = {"SET", key};
	return redis_command(conn, args, 2, data, size, NULL, NULL);
}

void
redis_disconnect(struct redis_connection *conn)
{
	if (!conn) {
		return;
	}
	if (conn->fd >= 0) {
		close(conn->fd);
	}
	free(conn->buffer);
	free(conn);
}

bool
redis_is_failed(struct redis_connection *conn)
{
	return conn->failed;
}

#else // _WIN32

// Redis remote storage is not supported on Windows.

struct redis_connection {
	int unused;
};

struct redis_connection *
redis_connect(const char *url)
{
	(void)url;
	cc_log("Redis: remote storage is not supported on Windows");
	return NULL;
}

int
redis_get(struct redis_connection *conn, const char *key,
          char **data, size_t *size)
{
	(void)conn; (void)key; (void)data; (void)size;
	return REDIS_RESULT_ERROR;
}

int
redis_set(struct redis_connection *conn, const char *key,
          const void *data, size_t size)
{
	(void)conn; (void)key; (void)data; (void)size;
	return REDIS_RESULT_ERROR;
}

void
redis_disconnect(struct redis_connection *conn)
{
	(void)conn;
}

bool
redis_is_failed(struct redis_connection *conn)
{
	(void)conn;
	return true;
}

#endif // _WIN32
