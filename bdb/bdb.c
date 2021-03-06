#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <db.h>
#include "rep_common.h"
#include "bdb.h"
#include "cmp.h"

void
log_error(const char *file, const char *function, int line, const char *msg, int err) {
    char buf[1024];
    int i, pos;

    i = 0;
    pos = 0;
    while (1) {
        if (file[i] == '\0') {
            break;
        }
#ifdef __WIN32
        if (file[i] == '\\') {
#else
        if (file[i] == '/') {
#endif
            if (file[i + 1] != '\0') {
                pos = i + 1;
            }
        }
        ++i;
    }

    snprintf(buf, sizeof buf, "%s|%d|%s|%s|%d|%s", file + pos, line, function, msg, err, db_strerror(err));
    buf[sizeof buf - 1] = 0;
    Error(buf);
}

int
db_close(DB *dbp) {
    int ret;
    if ((ret = dbp->close(dbp, 0)) != 0) {
        LOG_ERROR("close", ret);
    }
    return 0;
}

int is_finished(SHARED_DATA *shared_data) {
    return shared_data->app_finished;
}

int
db_get(DB *dbp, DB_TXN *txn, char *_key, unsigned int keylen, char **_data, unsigned int *datalen, unsigned int flags) {
    DBT key, data;
    int ret;

    memset(&key, 0, sizeof key);
    memset(&data, 0, sizeof data);

    key.data = _key;
    key.size = keylen;

    data.flags = DB_DBT_REALLOC;
    data.data = *_data;

    ret = dbp->get(dbp, txn, &key, &data, flags);
    if (ret == 0) {
        *_data = data.data;
        *datalen = data.size;
    } else if (ret == DB_NOTFOUND) {
        *_data = NULL;
        *datalen = 0;
    } else {
        LOG_ERROR("get", ret);
    }
    return ret;
}

int
txn_begin(DB_ENV *dbenv, DB_TXN **txn, unsigned int flags) {
    int ret;
    ret = dbenv->txn_begin(dbenv, NULL, txn, flags);
    if (ret) {
        LOG_ERROR("txn_begin", ret);
    }
}

int
txn_abort(DB_TXN *txn) {
    int ret;
    ret = txn->abort(txn);
    if (ret) {
        LOG_ERROR("abort", ret);
    }
}

int
txn_commit(DB_TXN *txn) {
    int ret;
    ret = txn->commit(txn, 0);
    if (ret) {
        LOG_ERROR("commit", ret);
    }
}

int
db_set_expire(
        DB *expire_db,
        DB *index_db,
        DB_TXN *txn,
        char *_key,
        unsigned int keylen,
        unsigned int sec,
        unsigned int seq,
        unsigned int tid) {
    DBT key, data;
    struct expire_key expire_value;
    int ret;

    memset(&key, 0, sizeof key);
    memset(&data, 0, sizeof data);

    time(&expire_value.t);
    expire_value.t += sec;
    expire_value.seq = seq;
    expire_value.thread_id = tid;

    key.data = &expire_value;
    key.size = sizeof expire_value;

    data.data = _key;
    data.size = keylen;

    ret = expire_db->put(expire_db, txn, &key, &data, DB_NOOVERWRITE);
    if (ret != 0) {
        LOG_ERROR("put|expire", ret);
        return ret;
    }
    ret = index_db->put(index_db, txn, &data, &key, 0);
    if (ret != 0) {
        LOG_ERROR("put|index", ret);
        return ret;
    }
    return 0;
}

int
db_put(DB *dbp, DB_TXN *txn, char *_key, unsigned int keylen, char *_data, unsigned int datalen, unsigned int flags) {
    DBT key, data;
    int ret;

    memset(&key, 0, sizeof key);
    memset(&data, 0, sizeof data);

    key.data = _key;
    key.size = keylen;

    data.data = _data;
    data.size = datalen;

    ret = dbp->put(dbp, txn, &key, &data, flags);
    return ret;
}

struct msgpack_reader_ctx {
    char *buf;
    int pos;
    int len;
};

static bool
msgpack_reader(cmp_ctx_t *ctx, void *data, size_t limit) {
    struct msgpack_reader_ctx *reader = (struct msgpack_reader_ctx*)ctx->buf;
    if (reader->pos + limit > reader->len) {
        return false;
    }
    memcpy(data, reader->buf + reader->pos, limit);
    reader->pos += limit;
    return true;
}

static int
msgpack_compare(cmp_ctx_t *cmp1, cmp_ctx_t *cmp2);

static int
msgpack_compare_array(cmp_ctx_t *cmp1, cmp_ctx_t *cmp2, int c) {
    int i;
    int r;

    for (i = 0; i < c; ++i) {
        r = msgpack_compare(cmp1, cmp2);
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

static int
msgpack_compare(cmp_ctx_t *cmp1, cmp_ctx_t *cmp2) {
    cmp_object_t obj1, obj2;
    int64_t r;
    char buf1[512], buf2[512];

    if (!cmp_read_object(cmp1, &obj1)) {
        Error("cmp1_read_object");
        return 0;
    }
    if (!cmp_read_object(cmp2, &obj2)) {
        Error("cmp2_read_object");
        return 0;
    }
    if (obj1.type != obj2.type) {
        Error("cmp_type_diff");
        return 0;
    }
    switch (obj1.type) {
        case CMP_TYPE_FIXARRAY:
        case CMP_TYPE_ARRAY16:
        case CMP_TYPE_ARRAY32:
            return msgpack_compare_array(cmp1, cmp2, obj1.as.array_size);
        case CMP_TYPE_SINT64:
            return obj1.as.s64 - obj1.as.s64;
        case CMP_TYPE_FIXSTR:
        case CMP_TYPE_STR8:
        case CMP_TYPE_STR16:
        case CMP_TYPE_STR32:
            if (obj1.as.str_size >= sizeof buf1 || obj2.as.str_size >= sizeof buf2) {
                Error("cmp_str_too_long");
                return 0;
            }
            msgpack_reader(cmp1, buf1, obj1.as.str_size);
            buf1[obj1.as.str_size] = 0;
            msgpack_reader(cmp2, buf2, obj2.as.str_size);
            buf2[obj2.as.str_size] = 0;
            return strcmp(buf1, buf2);
        default:
            Error("cmp|obj.type");
            return 0;
    }
}

static int
btree_key_compare(DB *db, const DBT *a, const DBT *b, size_t *locp) {
    int64_t r;
    struct msgpack_reader_ctx reader1, reader2;
    cmp_ctx_t cmp1, cmp2;

    reader1.buf = a->data;
    reader1.pos = 0;
    reader1.len = a->size;

    reader2.buf = b->data;
    reader2.pos = 0;
    reader2.len = b->size;

    cmp_init(&cmp1, &reader1, msgpack_reader, NULL);
    cmp_init(&cmp2, &reader2, msgpack_reader, NULL);

    return msgpack_compare(&cmp1, &cmp2);
}

static int
expire_key_compare(DB *db, const DBT *a, const DBT *b, size_t *locp) {
    struct expire_key *ai, *bi;
    int64_t r;

    ai = (struct expire_key*)a->data;
    bi = (struct expire_key*)b->data;

    r = ai->t - bi->t;
    if (r > 0) {
        return 1;
    } else if (r < 0) {
        return -1;
    }
    r = ai->seq - bi->seq;
    if (r > 0) {
        return 1;
    } else if (r < 0) {
        return -1;
    }
    r = ai->thread_id - bi->thread_id;
    if (r > 0) {
        return 1;
    } else if (r < 0) {
        return -1;
    } else {
        return 0;
    }
}

int
get_db(DB_ENV *dbenv, SHARED_DATA *shared_data, const char *name, int dbtype, DB **out) {
    DB *dbp;
    int ret, ret2;
	u_int32_t flags;
	permfail_t *pfinfo;


    *out = NULL;
    if ((ret = db_create(&dbp, dbenv, 0)) != 0)
        return (ret);

    flags = DB_AUTO_COMMIT | DB_READ_UNCOMMITTED | DB_THREAD;
    /*
     * Open database with DB_CREATE only if this is
     * a master database.  A client database uses
     * polling to attempt to open the database without
     * DB_CREATE until it is successful. 
     *
     * This DB_CREATE polling logic can be simplified
     * under some circumstances.  For example, if the
     * application can be sure a database is already
     * there, it would never need to open it with
     * DB_CREATE.
     */
    if (dbtype != DB_UNKNOWN && shared_data->is_master) {
        flags |= DB_CREATE;
    }
    if (strcmp("__expire.db", name) == 0) {
        ret = dbp->set_bt_compare(dbp, expire_key_compare);
        if (ret) {
            LOG_ERROR("set_dup_compare", ret);
            if ((ret2 = dbp->close(dbp, 0)) != 0) {
                LOG_ERROR("close", ret2);
            }
            return ret;
        }
    }
    if ((ret = dbp->open(dbp, NULL, name, NULL, dbtype, flags, 0)) != 0) {
        LOG_ERROR("open", ret);
        if ((ret2 = dbp->close(dbp, 0)) != 0) {
            LOG_ERROR("close", ret2);
        }
        return ret;
    }
    /* Check this thread's PERM_FAILED indicator. */
    *out = dbp;
    return 0;
}

#define DEFAULT_TABLE "__default"
void
split_key(char *_key, int keylen, char **table, int *tablelen, char **name, int *namelen) {
    int i;

    *name = NULL;
    *table = NULL;
	for (i = 0; i < keylen; ++i) {
		if (_key[i] == ':') {
            if (i != 0) {
                *table = _key;
                _key[i] = '\0';
                *tablelen = i;
            }
            *namelen = keylen - i - 1;
            if (*namelen != 0) {
                *name = _key + i + 1;
            }
			break;
		}
	}
	if (*table == NULL && *name == NULL) {
		*table = DEFAULT_TABLE;
        *tablelen = (sizeof DEFAULT_TABLE) - 1;
		*name = _key;
        *namelen = keylen;
	}
	if (table == NULL) {
		*table = DEFAULT_TABLE;
        *tablelen = (sizeof DEFAULT_TABLE) - 1;
	}
	if (*name == NULL) {
        *name = "\0";
        *namelen = 1;
	}
}

