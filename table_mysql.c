/*
 * Copyright (c) 2013 Eric Faurot <eric@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "compat.h"

#include <sys/tree.h>
#include <sys/types.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mysql/mysql.h>
#include <mysql/errmsg.h>

#include "dict.h"
#include "log.h"
#include "table_stdio.h"
#include "util.h"

enum {
	SQL_ALIAS = 0,
	SQL_DOMAIN,
	SQL_CREDENTIALS,
	SQL_NETADDR,
	SQL_USERINFO,
	SQL_SOURCE,
	SQL_MAILADDR,
	SQL_ADDRNAME,
	SQL_MAILADDRMAP,

	SQL_MAX
};

struct config {
	struct dict	 conf;
	MYSQL		*db;
	MYSQL_STMT	*statements[SQL_MAX];
	MYSQL_STMT	*stmt_fetch_source;
	struct dict	 sources;
	void		*source_iter;
	size_t		 source_refresh;
	size_t		 source_ncall;
	int		 source_expire;
	time_t		 source_update;
};

static MYSQL_STMT *table_mysql_query(const char *, int);

static void		 config_free(struct config *);

#define SQL_MAX_RESULT	5

#define	DEFAULT_EXPIRE	60
#define	DEFAULT_REFRESH	1000

static MYSQL_BIND	 results[SQL_MAX_RESULT];
static char		 results_buffer[SQL_MAX_RESULT][LINE_MAX];
static char		*conffile;
static struct config	*config;

static MYSQL_STMT *
table_mysql_prepare_stmt(MYSQL *_db, const char *query, unsigned long nparams,
    unsigned int nfields)
{
	MYSQL_STMT	*stmt;

	if ((stmt = mysql_stmt_init(_db)) == NULL) {
		log_warnx("warn: mysql_stmt_init: %s", mysql_error(_db));
		goto end;
	}
	if (mysql_stmt_prepare(stmt, query, strlen(query))) {
		log_warnx("warn: mysql_stmt_prepare: %s", mysql_stmt_error(stmt));
		goto end;
	}
	if (mysql_stmt_param_count(stmt) != nparams) {
		log_warnx("warn: wrong number of params for %s", query);
		goto end;
	}
	if (mysql_stmt_field_count(stmt) != nfields) {
		log_warnx("warn: wrong number of columns in resultset");
		goto end;
	}
	if (mysql_stmt_bind_result(stmt, results)) {
		log_warnx("warn: mysql_stmt_bind_results: %s", mysql_stmt_error(stmt));
		goto end;
	}

	return stmt;

    end:
	if (stmt)
		mysql_stmt_close(stmt);

	return NULL;
}

static struct config *
config_load(const char *path)
{
	struct config	*conf;
	FILE		*fp;
	size_t		 sz = 0;
	ssize_t		 flen;
	char		*key, *value, *buf = NULL;
	const char	*e;
	long long	 ll;

	if ((conf = calloc(1, sizeof(*conf))) == NULL) {
		log_warn("warn: calloc");
		return NULL;
	}

	dict_init(&conf->conf);
	dict_init(&conf->sources);

	conf->source_refresh = DEFAULT_REFRESH;
	conf->source_expire = DEFAULT_EXPIRE;

	if ((fp = fopen(path, "r")) == NULL) {
		log_warn("warn: \"%s\"", path);
		goto end;
	}

	while ((flen = getline(&buf, &sz, fp)) != -1) {
		if (buf[flen - 1] == '\n')
			buf[flen - 1] = '\0';

		key = strip(buf);
		if (*key == '\0' || *key == '#')
			continue;
		value = key;
		strsep(&value, " \t:");
		if (value) {
			while (*value) {
				if (!isspace((unsigned char)*value) &&
				    !(*value == ':' && isspace((unsigned char)*(value + 1))))
					break;
				++value;
			}
			if (*value == '\0')
				value = NULL;
		}

		if (value == NULL) {
			log_warnx("warn: missing value for key %s", key);
			goto end;
		}

		if (dict_check(&conf->conf, key)) {
			log_warnx("warn: duplicate key %s", key);
			goto end;
		}

		value = strdup(value);
		if (value == NULL) {
			log_warn("warn: strdup");
			goto end;
		}

		dict_set(&conf->conf, key, value);
	}

	if ((value = dict_get(&conf->conf, "fetch_source_expire"))) {
		e = NULL;
		ll = strtonum(value, 0, INT_MAX, &e);
		if (e) {
			log_warnx("warn: bad value for fetch_source_expire: %s", e);
			goto end;
		}
		conf->source_expire = ll;
	}
	if ((value = dict_get(&conf->conf, "fetch_source_refresh"))) {
		e = NULL;
		ll = strtonum(value, 0, INT_MAX, &e);
		if (e) {
			log_warnx("warn: bad value for fetch_source_refresh: %s", e);
			goto end;
		}
		conf->source_refresh = ll;
	}

	free(buf);
	fclose(fp);
	return conf;

    end:
	free(buf);
	fclose(fp);
	config_free(conf);
	return NULL;
}

static void
config_reset(struct config *conf)
{
	size_t	i;

	for (i = 0; i < SQL_MAX; i++) {
		if (conf->statements[i]) {
			mysql_stmt_close(conf->statements[i]);
			conf->statements[i] = NULL;
		}
	}
	if (conf->stmt_fetch_source) {
		mysql_stmt_close(conf->stmt_fetch_source);
		conf->stmt_fetch_source = NULL;
	}
	if (conf->db) {
		mysql_close(conf->db);
		conf->db = NULL;
	}
}

static int
config_connect(struct config *conf)
{
	static const struct {
		const char	*name;
		int		 cols;
	} qspec[SQL_MAX] = {
		{ "query_alias",	1 },
		{ "query_domain",	1 },
		{ "query_credentials",	2 },
		{ "query_netaddr",	1 },
		{ "query_userinfo",	3 },
		{ "query_source",	1 },
		{ "query_mailaddr",	1 },
		{ "query_addrname",	1 },
		{ "query_mailaddrmap",	1 },
	};
#if !defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 80001
	bool reconn;
#else
	my_bool	 reconn;
#endif
	size_t	 i;
	char	*host, *username, *password, *database, *q;

	log_debug("debug: (re)connecting");

	/* disconnect first, if needed */
	config_reset(conf);

	host = dict_get(&conf->conf, "host");
	username = dict_get(&conf->conf, "username");
	database = dict_get(&conf->conf, "database");
	password = dict_get(&conf->conf, "password");

	conf->db = mysql_init(NULL);
	if (conf->db == NULL) {
		log_warnx("warn: mysql_init failed");
		goto end;
	}

	reconn = 1;
	if (mysql_options(conf->db, MYSQL_OPT_RECONNECT, &reconn) != 0) {
		log_warnx("warn: mysql_options: %s", mysql_error(conf->db));
		goto end;
	}

	if (!mysql_real_connect(conf->db, host, username, password, database,
	    0, NULL, 0)) {
		log_warnx("warn: mysql_real_connect: %s", mysql_error(conf->db));
		goto end;
	}

	for (i = 0; i < SQL_MAX; i++) {
		q = dict_get(&conf->conf, qspec[i].name);
		if (q && (conf->statements[i] = table_mysql_prepare_stmt(
		    conf->db, q, 1, qspec[i].cols)) == NULL)
			goto end;
	}

	q = dict_get(&conf->conf, "fetch_source");
	if (q && (conf->stmt_fetch_source = table_mysql_prepare_stmt(conf->db,
	    q, 0, 1)) == NULL)
		goto end;

	log_debug("debug: connected");
	return 1;

end:
	config_reset(conf);
	return 0;
}

static void
config_free(struct config *conf)
{
	void	*value;

	config_reset(conf);

	while (dict_poproot(&conf->conf, &value))
		free(value);

	while (dict_poproot(&conf->sources, NULL))
		;

	free(conf);
}

static int
table_mysql_update(void)
{
	struct config	*c;

	if ((c = config_load(conffile)) == NULL)
		return 0;
	if (config_connect(c) == 0) {
		config_free(c);
		return 0;
	}

	config_free(config);
	config = c;

	return 1;
}

static MYSQL_STMT *
table_mysql_query(const char *key, int service)
{
	MYSQL_STMT	*stmt;
	MYSQL_BIND	 param[1];
	unsigned long	 keylen;
	char		 buffer[LINE_MAX];
	int		 i, retries = 1;

retry:
	stmt = NULL;
	for (i = 0; i < SQL_MAX; i++) {
		if (service == 1 << i) {
			stmt = config->statements[i];
			break;
		}
	}
	if (stmt == NULL)
		return NULL;

	if (strlcpy(buffer, key, sizeof(buffer)) >= sizeof(buffer)) {
		log_warnx("warn: key too long: \"%s\"", key);
		return NULL;
	}

	keylen = strlen(key);

	param[0].buffer_type = MYSQL_TYPE_STRING;
	param[0].buffer = buffer;
	param[0].buffer_length = sizeof(buffer);
	param[0].is_null = 0;
	param[0].length = &keylen;

	if (mysql_stmt_bind_param(stmt, param)) {
		log_warnx("warn: mysql_stmt_bind_param: %s", mysql_stmt_error(stmt));
		return NULL;
	}

	if (mysql_stmt_execute(stmt)) {
		if (mysql_stmt_errno(stmt)) {
			log_warnx("warn: trying to reconnect after error: %s", mysql_stmt_error(stmt));
			if (config_connect(config) && retries-- > 0)
				goto retry;
			if (retries <= 0)
				log_warnx("warn: to many retries");
			return NULL;
		}
		log_warnx("warn: mysql_stmt_execute: %s", mysql_stmt_error(stmt));
		return NULL;
	}
	return stmt;
}

static int
table_mysql_check(int service, struct dict *params, const char *key)
{
	MYSQL_STMT	*stmt;
	int		 r, s;

	if (config->db == NULL && config_connect(config) == 0)
		return -1;

	stmt = table_mysql_query(key, service);
	if (stmt == NULL)
		return -1;

	r = -1;
	s = mysql_stmt_fetch(stmt);

	if (s == 0)
		r = 1;
	else if (s == MYSQL_NO_DATA)
		r = 0;
	else
		log_warnx("warn: mysql_stmt_fetch: %s", mysql_stmt_error(stmt));

	if (mysql_stmt_free_result(stmt))
		log_warnx("warn: mysql_stmt_free_result: %s", mysql_stmt_error(stmt));
	return r;
}

static int
table_mysql_lookup(int service, struct dict *params, const char *key, char *dst, size_t sz)
{
	MYSQL_STMT	*stmt;
	int		 r, s;

	if (config->db == NULL && config_connect(config) == 0)
		return -1;

	if ((stmt = table_mysql_query(key, service)) == NULL)
		return -1;

	if ((s = mysql_stmt_fetch(stmt)) == MYSQL_NO_DATA) {
		r = 0;
		goto end;
	}

	if (s != 0) {
		r = -1;
		log_warnx("warn: mysql_stmt_fetch: %s", mysql_stmt_error(stmt));
		goto end;
	}

	r = 1;

	switch(service) {
	case K_ALIAS:
	case K_MAILADDRMAP:
		memset(dst, 0, sz);
		do {
			if (dst[0] && strlcat(dst, ", ", sz) >= sz) {
				log_warnx("warn: result too large");
				r = -1;
				break;
			}
			if (strlcat(dst, results_buffer[0], sz) >= sz) {
				log_warnx("warn: result too large");
				r = -1;
				break;
			}
			s = mysql_stmt_fetch(stmt);
		} while (s == 0);

		if (s && s != MYSQL_NO_DATA) {
			log_warnx("warn: mysql_stmt_fetch: %s", mysql_stmt_error(stmt));
			r = -1;
		}
		break;
	case K_CREDENTIALS:
		if (snprintf(dst, sz, "%s:%s",
		    results_buffer[0],
		    results_buffer[1]) > (ssize_t)sz) {
			log_warnx("warn: result too large");
			r = -1;
		}
		break;
	case K_USERINFO:
		if (snprintf(dst, sz, "%s:%s:%s",
		    results_buffer[0],
		    results_buffer[1],
		    results_buffer[2]) > (ssize_t)sz) {
			log_warnx("warn: result too large");
			r = -1;
		}
		break;
	case K_DOMAIN:
	case K_NETADDR:
	case K_SOURCE:
	case K_MAILADDR:
	case K_ADDRNAME:
		if (strlcpy(dst, results_buffer[0], sz) >= sz) {
			log_warnx("warn: result too large");
			r = -1;
		}
		break;
	default:
		log_warnx("warn: unknown service %d",
		    service);
		r = -1;
	}

end:
	if (mysql_stmt_free_result(stmt))
		log_warnx("warn: mysql_stmt_free_result: %s", mysql_stmt_error(stmt));
	return r;
}

static int
table_mysql_fetch(int service, struct dict *params, char *dst, size_t sz)
{
	MYSQL_STMT	*stmt;
	const char	*k;
	int		 s, retries = 1;

	if (config->db == NULL && config_connect(config) == 0)
		return -1;

retry:
	if (service != K_SOURCE)
		return -1;

	if ((stmt = config->stmt_fetch_source) == NULL)
		return -1;

	if (config->source_ncall < config->source_refresh &&
	    time(NULL) - config->source_update < config->source_expire)
		goto fetch;

	if (mysql_stmt_execute(stmt)) {
		if (mysql_stmt_errno(stmt)) {
			log_warnx("warn: trying to reconnect after error: %s", mysql_stmt_error(stmt));
			if (config_connect(config) && retries-- > 0)
				goto retry;
			if (retries <= 0)
				log_warnx("warn: to many retries");
			return -1;
		}
		log_warnx("warn: mysql_stmt_execute: %s", mysql_stmt_error(stmt));
		return -1;
	}

	config->source_iter = NULL;
	while (dict_poproot(&config->sources, NULL))
		;

	while ((s = mysql_stmt_fetch(stmt)) == 0)
		dict_set(&config->sources, results_buffer[0], NULL);

	if (s && s != MYSQL_NO_DATA)
		log_warnx("warn: mysql_stmt_fetch: %s", mysql_stmt_error(stmt));

	if (mysql_stmt_free_result(stmt))
		log_warnx("warn: mysql_stmt_free_result: %s",
		    mysql_stmt_error(stmt));

	config->source_update = time(NULL);
	config->source_ncall = 0;

fetch:
	config->source_ncall += 1;

	if (!dict_iter(&config->sources, &config->source_iter, &k, (void **)NULL)) {
		config->source_iter = NULL;
		if (!dict_iter(&config->sources, &config->source_iter, &k, (void **)NULL))
			return 0;
	}

	if (strlcpy(dst, k, sz) >= sz)
		return -1;

	return 1;
}

int
main(int argc, char **argv)
{
	int ch, i;

	log_init(1);
	log_setverbose(~0);

	while ((ch = getopt(argc, argv, "")) != -1) {
		switch (ch) {
		default:
			fatalx("bad option");
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1)
		fatalx("bogus argument(s)");

	conffile = argv[0];

	for (i = 0; i < SQL_MAX_RESULT; i++) {
		results[i].buffer_type = MYSQL_TYPE_STRING;
		results[i].buffer = results_buffer[i];
		results[i].buffer_length = LINE_MAX;
		results[i].is_null = 0;
	}

	if ((config = config_load(conffile)) == NULL)
		fatalx("error parsing config file");
	if (config_connect(config) == 0)
		fatalx("could not connect");

	table_api_on_update(table_mysql_update);
	table_api_on_check(table_mysql_check);
	table_api_on_lookup(table_mysql_lookup);
	table_api_on_fetch(table_mysql_fetch);
	table_api_dispatch();

	return 0;
}
