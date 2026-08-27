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

#ifndef REDIS_H
#define REDIS_H

#include <stdbool.h>
#include <stddef.h>

// Return codes for redis_get and redis_set.
enum redis_result {
	REDIS_RESULT_ERROR = -1,
	REDIS_RESULT_OK = 0,
	REDIS_RESULT_MISS = 1
};

// Which file of a cache entry a bundled file is. The values map to the file
// suffixes used in the cache directory.
enum redis_file_kind {
	REDIS_FILE_OBJ = 0,
	REDIS_FILE_STDERR = 1,
	REDIS_FILE_DEP = 2,
	REDIS_FILE_COV = 3,
	REDIS_FILE_SU = 4,
	REDIS_FILE_DIA = 5,
	REDIS_FILE_DWO = 6,
	REDIS_FILE_KIND_COUNT = 7
};

// Prefix of Redis keys holding cache entries, the same as in ccache 4.x.
#define REDIS_KEY_PREFIX "ccache:"

struct redis_connection;

// One file in a cache entry bundle.
struct redis_bundle_file {
	enum redis_file_kind kind;
	unsigned char *data;
	size_t size;
};

// A cache entry (the set of files sharing the same <hash>-<size> basename)
// encoded for storage in Redis.
struct redis_bundle {
	size_t n_files;
	struct redis_bundle_file *files;
};

// Parsed remote storage URL. See redis_parse_url for supported syntax.
struct redis_url {
	bool is_unix;
	char *host;        // TCP host (empty means localhost)
	int port;          // TCP port
	char *user;        // Redis ACL user, or NULL
	char *password;    // or NULL
	unsigned db;       // Redis database number
	char *socket_path; // Unix domain socket path (if is_unix)
};

// Connect to the Redis server at url. Returns NULL on failure (details are
// written to the log; the password is never logged).
struct redis_connection *redis_connect(const char *url);

// Get the value for key. On REDIS_RESULT_OK, *data/*size hold the value
// (caller frees). Returns REDIS_RESULT_MISS if the key doesn't exist.
int redis_get(struct redis_connection *conn, const char *key,
              char **data, size_t *size);

// Set key to data. Returns REDIS_RESULT_OK on success.
int redis_set(struct redis_connection *conn, const char *key,
              const void *data, size_t size);

void redis_disconnect(struct redis_connection *conn);

// Return true if the connection has failed and no more commands will be
// attempted on it.
bool redis_is_failed(struct redis_connection *conn);

// Parse a Redis storage URL on the same syntax form as ccache 4.x:
//
//   redis://[[USERNAME:]PASSWORD@]HOST[:PORT][/DBNUMBER]
//   redis+unix:SOCKET_PATH[?db=DBNUMBER]
//   redis+unix://[[USERNAME:]PASSWORD@localhost]SOCKET_PATH[?db=DBNUMBER]
//
// USERNAME, PASSWORD and SOCKET_PATH are percent-decoded. Returns false on
// malformed URLs (in which case *errmsg, if non-NULL, holds a reason; caller
// frees).
bool redis_parse_url(const char *url, struct redis_url *parsed, char **errmsg);
void redis_free_url(struct redis_url *parsed);

// Return a string representation of a parsed URL with credentials removed,
// suitable for logging. Caller frees.
char *redis_url_for_logging(const struct redis_url *url);

// Return the cache file suffix (".o", ".stderr", ...) for a file kind.
const char *redis_file_suffix(enum redis_file_kind kind);

// Encode a bundle of cache files. Caller frees *data. (Cannot fail since
// memory allocation aborts the process on out of memory.)
void redis_bundle_encode(const struct redis_bundle *bundle,
                         unsigned char **data, size_t *size);

// Decode an encoded bundle. Returns NULL if data is malformed. Caller frees
// the returned bundle with redis_bundle_free.
struct redis_bundle *redis_bundle_decode(const unsigned char *data, size_t size);

void redis_bundle_free(struct redis_bundle *bundle);

#endif // ifndef REDIS_H
