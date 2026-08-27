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

#include "../src/ccache.h"
#include "../src/redis.h"
#include "framework.h"

static bool
parse_url_ok(const char *url, struct redis_url *parsed)
{
	char *errmsg = NULL;
	bool ok = redis_parse_url(url, parsed, &errmsg);
	free(errmsg);
	return ok;
}

TEST_SUITE(redis)

TEST(redis_parse_url_tcp_defaults)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://localhost", &url));
	CHECK_STR_EQ("localhost", url.host);
	CHECK_INT_EQ(6379, url.port);
	CHECK_INT_EQ(0, url.db);
	CHECK(!url.is_unix);
	CHECK(!url.user);
	CHECK(!url.password);
	redis_free_url(&url);
}

TEST(redis_parse_url_tcp_full)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://cache.example.com:6380/2", &url));
	CHECK_STR_EQ("cache.example.com", url.host);
	CHECK_INT_EQ(6380, url.port);
	CHECK_INT_EQ(2, url.db);
	CHECK(!url.password);
	redis_free_url(&url);
}

TEST(redis_parse_url_tcp_password)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://secret@localhost:6379/0", &url));
	CHECK_STR_EQ("localhost", url.host);
	CHECK_INT_EQ(6379, url.port);
	CHECK(!url.user);
	CHECK_STR_EQ("secret", url.password);
	redis_free_url(&url);
}

TEST(redis_parse_url_tcp_user_password)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://user:pw@localhost/1", &url));
	CHECK_STR_EQ("user", url.user);
	CHECK_STR_EQ("pw", url.password);
	CHECK_INT_EQ(1, url.db);
	redis_free_url(&url);
}

TEST(redis_parse_url_percent_decoding)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://p%40ss@localhost", &url));
	CHECK_STR_EQ("p@ss", url.password);
	redis_free_url(&url);

	// A percent-decoded colon splits user and password (same as in ccache 4.x
	// where userinfo is decoded before splitting).
	CHECK(parse_url_ok("redis://p%40ss%3Aword@localhost", &url));
	CHECK_STR_EQ("p@ss", url.user);
	CHECK_STR_EQ("word", url.password);
	redis_free_url(&url);
}

TEST(redis_parse_url_unix)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis+unix:/run/redis.sock", &url));
	CHECK(url.is_unix);
	CHECK_STR_EQ("/run/redis.sock", url.socket_path);
	CHECK_INT_EQ(0, url.db);
	CHECK(!url.password);
	redis_free_url(&url);
}

TEST(redis_parse_url_unix_db)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis+unix:/run/redis.sock?db=3", &url));
	CHECK(url.is_unix);
	CHECK_STR_EQ("/run/redis.sock", url.socket_path);
	CHECK_INT_EQ(3, url.db);
	redis_free_url(&url);
}

TEST(redis_parse_url_unix_authority)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis+unix://user:pw@localhost/run/redis.sock?db=4",
	                   &url));
	CHECK(url.is_unix);
	CHECK_STR_EQ("/run/redis.sock", url.socket_path);
	CHECK_STR_EQ("user", url.user);
	CHECK_STR_EQ("pw", url.password);
	CHECK_INT_EQ(4, url.db);
	redis_free_url(&url);
}

TEST(redis_parse_url_unix_empty_authority)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis+unix:///run/redis.sock", &url));
	CHECK(url.is_unix);
	CHECK_STR_EQ("/run/redis.sock", url.socket_path);
	redis_free_url(&url);
}

TEST(redis_parse_url_invalid)
{
	struct redis_url url;
	char *errmsg = NULL;
	CHECK(!redis_parse_url("http://localhost", &url, &errmsg));
	CHECK_STR_EQ_FREE2(
		"URL must start with \"redis://\" or \"redis+unix:\"", errmsg);
	errmsg = NULL;
	CHECK(!redis_parse_url("redis://host:99999", &url, &errmsg));
	CHECK(errmsg);
	free(errmsg);
	errmsg = NULL;
	CHECK(!redis_parse_url("redis://host/xyz", &url, &errmsg));
	CHECK(errmsg);
	free(errmsg);
	errmsg = NULL;
	CHECK(!redis_parse_url("redis+unix:_relative/path", &url, &errmsg));
	CHECK(errmsg);
	free(errmsg);
	errmsg = NULL;
	CHECK(!redis_parse_url("redis+unix://user@otherhost/sock", &url, &errmsg));
	CHECK(errmsg);
	free(errmsg);
	errmsg = NULL;
	CHECK(!redis_parse_url("redis://host:0", &url, &errmsg));
	CHECK(errmsg);
	free(errmsg);
}

TEST(redis_url_for_logging_redacts_credentials)
{
	struct redis_url url;
	CHECK(parse_url_ok("redis://user:pw@host:6380/2", &url));
	char *desc = redis_url_for_logging(&url);
	CHECK_STR_EQ_FREE2("redis://host:6380/2", desc);
	redis_free_url(&url);

	CHECK(parse_url_ok("redis://user:pw@localhost", &url));
	desc = redis_url_for_logging(&url);
	CHECK_STR_EQ_FREE2("redis://localhost:6379", desc);
	redis_free_url(&url);

	CHECK(parse_url_ok("redis+unix://user:pw@localhost/run/x.sock", &url));
	desc = redis_url_for_logging(&url);
	CHECK_STR_EQ_FREE2("redis+unix:/run/x.sock", desc);
	redis_free_url(&url);
}

TEST(redis_bundle_roundtrip)
{
	struct redis_bundle *bundle = x_malloc(sizeof(*bundle));
	bundle->n_files = 3;
	bundle->files = x_calloc(3, sizeof(*bundle->files));
	bundle->files[0].kind = REDIS_FILE_OBJ;
	bundle->files[0].data = (unsigned char *)x_strdup("object data");
	bundle->files[0].size = 11;
	bundle->files[1].kind = REDIS_FILE_STDERR;
	bundle->files[1].data = (unsigned char *)x_strdup("warning!");
	bundle->files[1].size = 8;
	bundle->files[2].kind = REDIS_FILE_DWO;
	// Binary data including NUL and CR/LF bytes.
	unsigned char binary[] = {0, 1, '\r', '\n', 0xff, 0xfe};
	bundle->files[2].data = (unsigned char *)x_malloc(sizeof(binary));
	memcpy(bundle->files[2].data, binary, sizeof(binary));
	bundle->files[2].size = sizeof(binary);

	unsigned char *data = NULL;
	size_t size = 0;
	redis_bundle_encode(bundle, &data, &size);

	struct redis_bundle *decoded = redis_bundle_decode(data, size);
	CHECK(decoded);
	CHECK_INT_EQ(3, (int)decoded->n_files);
	CHECK_INT_EQ(REDIS_FILE_OBJ, decoded->files[0].kind);
	CHECK_INT_EQ(11, (int)decoded->files[0].size);
	CHECK(memcmp(bundle->files[0].data, decoded->files[0].data, 11) == 0);
	CHECK_INT_EQ(REDIS_FILE_STDERR, decoded->files[1].kind);
	CHECK(memcmp(bundle->files[1].data, decoded->files[1].data, 8) == 0);
	CHECK_INT_EQ(REDIS_FILE_DWO, decoded->files[2].kind);
	CHECK_INT_EQ((int)sizeof(binary), (int)decoded->files[2].size);
	CHECK(memcmp(binary, decoded->files[2].data, sizeof(binary)) == 0);

	free(data);
	redis_bundle_free(decoded);
	redis_bundle_free(bundle);
}

TEST(redis_bundle_decode_rejects_bad_data)
{
	struct redis_bundle *bundle = x_malloc(sizeof(*bundle));
	bundle->n_files = 1;
	bundle->files = x_calloc(1, sizeof(*bundle->files));
	bundle->files[0].kind = REDIS_FILE_OBJ;
	bundle->files[0].data = (unsigned char *)x_strdup("x");
	bundle->files[0].size = 1;

	unsigned char *data = NULL;
	size_t size = 0;
	redis_bundle_encode(bundle, &data, &size);

	// Wrong magic.
	unsigned char *bad = x_malloc(size);
	memcpy(bad, data, size);
	bad[0] = 'X';
	CHECK(!redis_bundle_decode(bad, size));
	free(bad);

	// Wrong version.
	bad = x_malloc(size);
	memcpy(bad, data, size);
	bad[4] = 99;
	CHECK(!redis_bundle_decode(bad, size));
	free(bad);

	// Truncated data.
	CHECK(!redis_bundle_decode(data, size - 1));

	// Trailing garbage.
	bad = x_malloc(size + 1);
	memcpy(bad, data, size);
	bad[size] = 0;
	CHECK(!redis_bundle_decode(bad, size + 1));
	free(bad);

	// Unknown file kind.
	bad = x_malloc(size);
	memcpy(bad, data, size);
	bad[9] = 99;
	CHECK(!redis_bundle_decode(bad, size));
	free(bad);

	// Too short for the header.
	CHECK(!redis_bundle_decode(data, 8));

	free(data);
	redis_bundle_free(bundle);
}

TEST(redis_file_suffix_mapping)
{
	CHECK(str_eq(".o", redis_file_suffix(REDIS_FILE_OBJ)));
	CHECK(str_eq(".stderr", redis_file_suffix(REDIS_FILE_STDERR)));
	CHECK(str_eq(".d", redis_file_suffix(REDIS_FILE_DEP)));
	CHECK(str_eq(".gcno", redis_file_suffix(REDIS_FILE_COV)));
	CHECK(str_eq(".su", redis_file_suffix(REDIS_FILE_SU)));
	CHECK(str_eq(".dia", redis_file_suffix(REDIS_FILE_DIA)));
	CHECK(str_eq(".dwo", redis_file_suffix(REDIS_FILE_DWO)));
}

TEST_SUITE_END
