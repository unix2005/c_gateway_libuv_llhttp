/*
 * Oracle 驱动（基于 ODPI-C，官方 Oracle C 驱动）。
 *
 * 设计要点：
 *   - 完全实现 db.h 定义的 q_db_ops_t，上层 mapper / 连接池 / 事务零改动即可用。
 *   - mapper 生成的 SQL 使用 '?' 占位符，prepare/exec 时统一改写为 Oracle 的
 *     位置绑定 ':1' ':2' ...（跳过字符串字面量里的 '?'）。
 *   - 预处理语句缓存在连接私有数据（conn->ud）上；结果集 res 只是对缓存语句的
 *     "借用"（own=0），res_free 不释放语句，语句由 q_stmt_close 在连接销毁时释放。
 *     仅 ad-hoc exec 产生的临时结果 own=1，由 res_free 释放。
 *   - 事务沿用通用层的 "START TRANSACTION / COMMIT / ROLLBACK" 文本：
 *     Oracle 自动起事务，故 START TRANSACTION 视为 no-op；COMMIT/ROLLBACK 映射到
 *     dpiConn_commit / dpiConn_rollback。
 *   - auto-increment：Oracle 用序列/RETURNING INTO，普通 INSERT 取不到自增 id，
 *     此处 insert_id 固定返回 0（如需返回可用 RETURNING，超出本期范围）。
 */

#include "q_db.h"

#include <dpi.h>

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------- 数据结构 ---------------- */

typedef struct
{
    dpiConn *conn;
} oracle_conn_t;

typedef struct
{
    dpiStmt *stmt; /* 结果集对应的语句（可能属于缓存的 q_stmt） */
    int own;       /* 1=res 负责释放 stmt（ad-hoc exec）；0=stmt 属于缓存的 q_stmt */
    int ncols;
    char **names;       /* 列名副本，res_free 释放 */
    dpiQueryInfo *info; /* 列类型信息，res_free 释放 */
} oracle_res_t;

/* ---------------- 全局 ODPI-C 上下文 ---------------- */

static dpiContext *g_ctx = NULL;
static pthread_mutex_t g_ctx_mu = PTHREAD_MUTEX_INITIALIZER;

static dpiContext *oc_get_context(char *err, size_t errlen)
{
    pthread_mutex_lock(&g_ctx_mu);
    if (g_ctx == NULL)
    {
        dpiErrorInfo ei;
        if (dpiContext_create(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &g_ctx, &ei) != DPI_SUCCESS)
        {
            snprintf(err, errlen, "oracle init: %.*s", (int)ei.messageLength, ei.message ? ei.message : "");
            g_ctx = NULL;
        }
    }
    pthread_mutex_unlock(&g_ctx_mu);
    return g_ctx;
}

static int oc_error(dpiContext *ctx, char *err, size_t errlen, const char *what)
{
    dpiErrorInfo ei;
    if (ctx != NULL)
        dpiContext_getError(ctx, &ei);
    else
    {
        ei.message = "";
        ei.messageLength = 0;
        ei.fnName = "";
        ei.code = 0;
    }
    snprintf(err, errlen, "%s: %.*s (fn=%s code=%d)", what ? what : "oracle",
             (int)(ei.messageLength ? ei.messageLength : 0), ei.message ? ei.message : "", ei.fnName ? ei.fnName : "",
             (int)ei.code);
    return Q_ERR;
}

/* ---------------- SQL 占位符翻译：? -> :1 :2 ... ---------------- */

/* 在字符串字面量之外把 '?' 改成位置绑定 ':N'。返回动态分配的新 SQL，*np 为参数个数。 */
static char *oc_translate_sql(const char *sql, size_t len, int *np)
{
    size_t cap = len * 2 + 16;
    char *out = malloc(cap);
    if (out == NULL)
    {
        *np = 0;
        return NULL;
    }

    size_t o = 0;
    int q = 0; /* 0=不在引号内，否则为引号字符 */
    int n = 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = sql[i];
        if (q != 0)
        {
            if (o < cap - 1)
                out[o++] = c;
            if (c == q)
                q = 0;
            continue;
        }
        if (c == '\'' || c == '"')
        {
            q = c;
            if (o < cap - 1)
                out[o++] = c;
            continue;
        }
        if (c == '?')
        {
            int wrote = snprintf(out + o, cap - o, ":%d", ++n);
            if (wrote > 0)
                o += (size_t)wrote;
        }
        else
        {
            if (o < cap - 1)
                out[o++] = c;
        }
    }
    if (o < cap)
        out[o] = '\0';
    else
        out[cap - 1] = '\0';
    *np = n;
    return out;
}

/* ---------------- 列类型判断 ---------------- */

static int oc_is_char(dpiOracleTypeNum t)
{
    switch (t)
    {
    case DPI_ORACLE_TYPE_VARCHAR:
    case DPI_ORACLE_TYPE_NVARCHAR:
    case DPI_ORACLE_TYPE_CHAR:
    case DPI_ORACLE_TYPE_NCHAR:
    case DPI_ORACLE_TYPE_ROWID:
    case DPI_ORACLE_TYPE_CLOB:
    case DPI_ORACLE_TYPE_NCLOB:
    case DPI_ORACLE_TYPE_LONG_VARCHAR:
    case DPI_ORACLE_TYPE_DATE:
    case DPI_ORACLE_TYPE_TIMESTAMP:
    case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
    case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
    case DPI_ORACLE_TYPE_INTERVAL_DS:
    case DPI_ORACLE_TYPE_INTERVAL_YM:
        return 1;
    default:
        return 0;
    }
}

static int oc_is_raw(dpiOracleTypeNum t)
{
    switch (t)
    {
    case DPI_ORACLE_TYPE_RAW:
    case DPI_ORACLE_TYPE_BLOB:
    case DPI_ORACLE_TYPE_BFILE:
    case DPI_ORACLE_TYPE_LONG_RAW:
        return 1;
    default:
        return 0;
    }
}

/* ---------------- 驱动实现 ---------------- */

static int oc_connect(void **out, const q_dsn_t *dsn, char *err, size_t errlen)
{
    dpiContext *ctx = oc_get_context(err, errlen);
    if (ctx == NULL)
        return Q_ERR;

    oracle_conn_t *c = calloc(1, sizeof(oracle_conn_t));
    if (c == NULL)
    {
        snprintf(err, errlen, "oracle: oom");
        return Q_ERR_NOMEM;
    }

    /* EZConnect：//host:port/service_name，无 service 时退化为 host:port */
    char ez[512];
    if (dsn->name[0] != '\0')
        snprintf(ez, sizeof(ez), "//%s:%d/%s", dsn->host, dsn->port, dsn->name);
    else
        snprintf(ez, sizeof(ez), "%s:%d", dsn->host, dsn->port);

    dpiCommonCreateParams cp;
    memset(&cp, 0, sizeof(cp));
    cp.createMode = DPI_MODE_CREATE_THREADED; /* 句柄跨线程安全（每条连接单线程使用） */
    cp.encoding = "UTF-8";
    cp.nencoding = "UTF-8";
    cp.driverName = "cservice";

    dpiConnCreateParams ccp;
    memset(&ccp, 0, sizeof(ccp));

    dpiConn *conn = NULL;
    if (dpiConn_create(ctx, dsn->user, (uint32_t)strlen(dsn->user), dsn->pass, (uint32_t)strlen(dsn->pass), ez,
                       (uint32_t)strlen(ez), &cp, &ccp, &conn) != DPI_SUCCESS)
    {
        oc_error(ctx, err, errlen, "oracle connect");
        free(c);
        return Q_ERR;
    }
    c->conn = conn;
    *out = c;
    return Q_OK;
}

static void oc_close(void *h)
{
    oracle_conn_t *c = h;
    if (c == NULL)
        return;
    if (c->conn != NULL)
        dpiConn_release(c->conn);
    free(c);
}

static int oc_ping(void *h, char *err, size_t errlen)
{
    oracle_conn_t *c = h;
    if (c == NULL || c->conn == NULL)
        return Q_ERR_INVAL;
    if (dpiConn_ping(c->conn) != DPI_SUCCESS)
        return oc_error(g_ctx, err, errlen, "oracle ping");
    return Q_OK;
}

/* 事务关键字特判：返回 1 表示已处理（调用方应视为成功），0 表示需正常执行 */
static int oc_tx_keyword(const char *sql, uint32_t *act)
{
    if (strncasecmp(sql, "START TRANSACTION", 17) == 0)
    {
        *act = 0;
        return 1;
    }
    if (strncasecmp(sql, "COMMIT", 6) == 0)
    {
        *act = 1;
        return 1;
    }
    if (strncasecmp(sql, "ROLLBACK", 8) == 0)
    {
        *act = 2;
        return 1;
    }
    return 0;
}

static int oc_exec(void *h, const char *sql, size_t len, void **res, uint64_t *affected, uint64_t *insert_id, char *err,
                   size_t errlen)
{
    oracle_conn_t *c = h;
    if (c == NULL || c->conn == NULL)
        return Q_ERR_INVAL;
    if (res != NULL)
        *res = NULL;

    /* 事务控制语句 */
    uint32_t act = 0;
    if (oc_tx_keyword(sql, &act))
    {
        if (act == 1)
        {
            if (dpiConn_commit(c->conn) != DPI_SUCCESS)
                return oc_error(g_ctx, err, errlen, "oracle commit");
        }
        else if (act == 2)
        {
            if (dpiConn_rollback(c->conn) != DPI_SUCCESS)
                return oc_error(g_ctx, err, errlen, "oracle rollback");
        }
        /* START TRANSACTION：Oracle 自动起事务，no-op */
        if (affected)
            *affected = 0;
        if (insert_id)
            *insert_id = 0;
        return Q_OK;
    }

    int np = 0;
    char *ts = oc_translate_sql(sql, len, &np);
    if (ts == NULL)
    {
        snprintf(err, errlen, "oracle: oom");
        return Q_ERR_NOMEM;
    }

    dpiStmt *s = NULL;
    int rc = Q_OK;
    if (dpiConn_prepareStmt(c->conn, 0, ts, (uint32_t)strlen(ts), NULL, 0, &s) != DPI_SUCCESS)
    {
        rc = oc_error(g_ctx, err, errlen, "oracle prepare");
        goto done;
    }

    uint32_t ncols = 0;
    if (dpiStmt_execute(s, DPI_MODE_EXEC_DEFAULT, &ncols) != DPI_SUCCESS)
    {
        rc = oc_error(g_ctx, err, errlen, "oracle exec");
        dpiStmt_release(s);
        goto done;
    }

    uint64_t rowcount = 0;
    dpiStmt_getRowCount(s, &rowcount);
    if (affected)
        *affected = rowcount;
    if (insert_id)
        *insert_id = 0; /* Oracle 无自增，需 RETURNING/序列，本期不取 */

    if (ncols > 0 && res != NULL)
    {
        oracle_res_t *r = calloc(1, sizeof(oracle_res_t));
        if (r == NULL)
        {
            dpiStmt_release(s);
            snprintf(err, errlen, "oracle: oom");
            rc = Q_ERR_NOMEM;
            goto done;
        }
        r->stmt = s;
        r->own = 1; /* ad-hoc 查询：res 负责释放语句 */
        *res = r;
    }
    else
    {
        dpiStmt_release(s); /* 非查询或调用方不要结果集，立即释放 */
    }

done:
    free(ts);
    return rc;
}

static int oc_prepare(void *h, void **stmt, const char *sql, size_t len, int *nparams, char *err, size_t errlen)
{
    oracle_conn_t *c = h;
    if (c == NULL || c->conn == NULL)
        return Q_ERR_INVAL;

    int np = 0;
    char *ts = oc_translate_sql(sql, len, &np);
    if (ts == NULL)
    {
        snprintf(err, errlen, "oracle: oom");
        return Q_ERR_NOMEM;
    }

    dpiStmt *s = NULL;
    int rc = Q_OK;
    if (dpiConn_prepareStmt(c->conn, 0, ts, (uint32_t)strlen(ts), NULL, 0, &s) != DPI_SUCCESS)
    {
        rc = oc_error(g_ctx, err, errlen, "oracle prepare");
    }
    else
    {
        *stmt = s;
        if (nparams != NULL)
            *nparams = np;
    }
    free(ts);
    return rc;
}

static void oc_stmt_close(void *stmt)
{
    if (stmt != NULL)
        dpiStmt_release((dpiStmt *)stmt);
}

static int oc_stmt_bind(void *stmt, const q_value_t *params, int n, char *err, size_t errlen)
{
    dpiStmt *s = stmt;
    if (s == NULL || params == NULL)
        return Q_ERR_INVAL;

    for (int i = 0; i < n; i++)
    {
        dpiData d;
        memset(&d, 0, sizeof(d));
        dpiNativeTypeNum nt = DPI_NATIVE_TYPE_BYTES;

        if (params[i].type == Q_VAL_NULL)
        {
            d.isNull = 1;
        }
        else
        {
            switch (params[i].type)
            {
            case Q_VAL_INT:
                d.value.asInt64 = params[i].i64;
                nt = DPI_NATIVE_TYPE_INT64;
                break;
            case Q_VAL_DOUBLE:
                d.value.asDouble = params[i].dbl;
                nt = DPI_NATIVE_TYPE_DOUBLE;
                break;
            case Q_VAL_STRING:
            case Q_VAL_BLOB:
            {
                dpiBytes b;
                memset(&b, 0, sizeof(b));
                b.ptr = (char *)params[i].str;
                b.length = (uint32_t)params[i].len;
                b.encoding = NULL;
                d.value.asBytes = b;
                nt = DPI_NATIVE_TYPE_BYTES;
                break;
            }
            default:
                d.isNull = 1;
                break;
            }
        }
        if (dpiStmt_bindValueByPos(s, (uint32_t)(i + 1), nt, &d) != DPI_SUCCESS)
            return oc_error(g_ctx, err, errlen, "oracle bind");
    }
    return Q_OK;
}

static int oc_stmt_exec(void *stmt, void **res, uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen)
{
    dpiStmt *s = stmt;
    if (s == NULL)
        return Q_ERR_INVAL;
    if (res != NULL)
        *res = NULL;

    uint32_t ncols = 0;
    if (dpiStmt_execute(s, DPI_MODE_EXEC_DEFAULT, &ncols) != DPI_SUCCESS)
        return oc_error(g_ctx, err, errlen, "oracle stmt exec");

    uint64_t rowcount = 0;
    dpiStmt_getRowCount(s, &rowcount);
    if (affected)
        *affected = rowcount;
    if (insert_id)
        *insert_id = 0;

    if (ncols > 0 && res != NULL)
    {
        oracle_res_t *r = calloc(1, sizeof(oracle_res_t));
        if (r == NULL)
        {
            snprintf(err, errlen, "oracle: oom");
            return Q_ERR_NOMEM;
        }
        r->stmt = s;
        r->own = 0; /* 借用缓存的 q_stmt，res_free 不释放语句 */
        *res = r;
    }
    /* ncols==0 或不要结果集：语句留在缓存中供复用，不释放 */
    return Q_OK;
}

static int oc_res_cols(void *res, const char ***names, int *ncols)
{
    oracle_res_t *r = res;
    if (r == NULL || r->stmt == NULL)
        return Q_ERR_INVAL;

    uint32_t n = 0;
    if (dpiStmt_getNumQueryColumns(r->stmt, &n) != DPI_SUCCESS)
        return oc_error(g_ctx, NULL, 0, "oracle res_cols");
    r->ncols = (int)n;
    r->names = calloc(n ? n : 1, sizeof(char *));
    r->info = calloc(n ? n : 1, sizeof(dpiQueryInfo));
    if (r->names == NULL || r->info == NULL)
        return Q_ERR_NOMEM;

    for (uint32_t i = 0; i < n; i++)
    {
        dpiQueryInfo qi;
        if (dpiStmt_getQueryInfo(r->stmt, i + 1, &qi) == DPI_SUCCESS)
        {
            r->info[i] = qi;
            r->names[i] = malloc(qi.nameLength + 1);
            if (r->names[i] != NULL)
            {
                memcpy(r->names[i], qi.name, qi.nameLength);
                r->names[i][qi.nameLength] = '\0';
            }
            else
            {
                r->names[i] = strdup("");
            }
        }
        else
        {
            r->names[i] = strdup("");
        }
    }
    *names = (const char **)r->names;
    *ncols = r->ncols;
    return Q_OK;
}

static int oc_res_row(void *res, q_value_t *vals, int ncols, char *err, size_t errlen)
{
    oracle_res_t *r = res;
    if (r == NULL || r->stmt == NULL)
        return Q_ERR_INVAL;

    int found = 0;
    uint32_t bufferRowIndex = 0;
    if (dpiStmt_fetch(r->stmt, &found, &bufferRowIndex) != DPI_SUCCESS)
        return oc_error(g_ctx, err, errlen, "oracle fetch");
    if (!found)
        return 0; /* 结果集结束 */

    int n = (ncols < r->ncols) ? ncols : r->ncols;
    for (int i = 0; i < n; i++)
    {
        memset(&vals[i], 0, sizeof(vals[i]));

        dpiOracleTypeNum ot = r->info[i].typeInfo.oracleTypeNum;
        dpiNativeTypeNum nt;
        if (oc_is_char(ot))
            nt = DPI_NATIVE_TYPE_BYTES;
        else if (ot == DPI_ORACLE_TYPE_NUMBER)
            nt = (r->info[i].typeInfo.scale == 0) ? DPI_NATIVE_TYPE_INT64 : DPI_NATIVE_TYPE_DOUBLE;
        else if (ot == DPI_ORACLE_TYPE_NATIVE_FLOAT || ot == DPI_ORACLE_TYPE_NATIVE_DOUBLE)
            nt = DPI_NATIVE_TYPE_DOUBLE;
        else
            nt = DPI_NATIVE_TYPE_BYTES;

        dpiData *data = NULL;
        if (dpiStmt_getQueryValue(r->stmt, (uint32_t)(i + 1), &nt, &data) != DPI_SUCCESS || data == NULL)
        {
            vals[i].type = Q_VAL_NULL;
            continue;
        }
        if (data->isNull)
        {
            vals[i].type = Q_VAL_NULL;
            continue;
        }

        if (nt == DPI_NATIVE_TYPE_INT64)
        {
            vals[i].type = Q_VAL_INT;
            vals[i].i64 = data->value.asInt64;
        }
        else if (nt == DPI_NATIVE_TYPE_DOUBLE)
        {
            vals[i].type = Q_VAL_DOUBLE;
            vals[i].dbl = data->value.asDouble;
        }
        else
        {
            int blob = oc_is_raw(ot);
            vals[i].type = blob ? Q_VAL_BLOB : Q_VAL_STRING;
            vals[i].str = data->value.asBytes.ptr; /* 指向 ODPI-C 内部缓冲，下次 fetch 前消费 */
            vals[i].len = data->value.asBytes.length;
        }
    }
    return 1;
}

static void oc_res_free(void *res)
{
    oracle_res_t *r = res;
    if (r == NULL)
        return;
    if (r->own && r->stmt != NULL)
        dpiStmt_release(r->stmt); /* 仅 ad-hoc 查询需要释放语句 */
    for (int i = 0; i < r->ncols; i++)
        free(r->names[i]);
    free(r->names);
    free(r->info);
    free(r);
}

/* ---------------- 方言 ---------------- */

static void oracle_paginate(char *buf, size_t cap, long long offset, long long limit)
{
    if (limit <= 0)
        snprintf(buf, cap, " OFFSET %lld ROWS", offset);
    else
        snprintf(buf, cap, " OFFSET %lld ROWS FETCH NEXT %lld ROWS ONLY", offset, limit);
}

static const q_dialect_t oracle_dialect = {.name = "oracle",
                                           .placeholder = '?', /* mapper 仍用 '?'，prepare 内改写为 ':N' */
                                           .support_returning = 1,
                                           .paginate = oracle_paginate};

static const q_db_ops_t oracle_ops = {.connect = oc_connect,
                                      .close = oc_close,
                                      .ping = oc_ping,
                                      .exec = oc_exec,
                                      .prepare = oc_prepare,
                                      .stmt_close = oc_stmt_close,
                                      .stmt_bind = oc_stmt_bind,
                                      .stmt_exec = oc_stmt_exec,
                                      .res_cols = oc_res_cols,
                                      .res_row = oc_res_row,
                                      .res_free = oc_res_free};

static const q_db_driver_t oracle_driver = {.name = "oracle", .ops = &oracle_ops, .dialect = &oracle_dialect};

/**
 * @brief 注册 Oracle 驱动（ODPI-C）
 * @details 在创建 oracle:// DSN 的连接池之前调用一次；驱动以 "oracle" 为名字注册，
 *          q_db_find("oracle") 后即可被连接池选用。
 * @return Q_OK 成功，Q_ERR_EXIST 已注册，Q_ERR 失败
 */
int q_db_register_oracle(void) { return q_db_register(&oracle_driver); }
