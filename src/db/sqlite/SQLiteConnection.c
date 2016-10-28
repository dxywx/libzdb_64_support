/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.
 */


#include "Config.h"

#include <stdio.h>
#include <sqlite3.h>

#include "URL.h"
#include "ResultSet.h"
#include "StringBuffer.h"
#include "system/Time.h"
#include "PreparedStatement.h"
#include "SQLiteResultSet.h"
#include "SQLitePreparedStatement.h"
#include "ConnectionDelegate.h"
#include "SQLiteConnection.h"


/**
 * Implementation of the Connection/Delegate interface for SQLite 
 *
 * @file
 */


/* ----------------------------------------------------------- Definitions */


#define T ConnectionDelegate_T
struct T {
        Connection_T delegator;
	sqlite3 *db;
	int maxRows;
	int timeout;
	int lastError;
        StringBuffer_T sb;
};

extern const struct Rop_T sqlite3rops;
extern const struct Pop_T sqlite3pops;


/* ------------------------------------------------------- Private methods */


static sqlite3 *_doConnect(Connection_T delegator, char **error) {
        int status;
	sqlite3 *db;
        const char *path = URL_getPath(Connection_getURL(delegator));
        if (! path) {
                *error = Str_dup("no database specified in URL");
                return NULL;
        }
        /* Shared cache mode help reduce database lock problems if libzdb is used with many threads */
#if SQLITE_VERSION_NUMBER >= 3005000
#ifndef DARWIN
        /*
         SQLite doc e.al.: "sqlite3_enable_shared_cache is disabled on MacOS X 10.7 and iOS version 5.0 and
         will always return SQLITE_MISUSE. On those systems, shared cache mode should be enabled
         per-database connection via sqlite3_open_v2() with SQLITE_OPEN_SHAREDCACHE".
         As of OS X 10.10.4 this method is still deprecated and it is unclear if the recomendation above
         holds as SQLite from 3.5 requires that both sqlite3_enable_shared_cache() _and_
         sqlite3_open_v2(SQLITE_OPEN_SHAREDCACHE) is used to enable shared cache (!).
         */
        sqlite3_enable_shared_cache(true);
#endif
        status = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_SHAREDCACHE, NULL);
#else
        status = sqlite3_open(path, &db);
#endif
        if (SQLITE_OK != status) {
                *error = Str_cat("cannot open database '%s' -- %s", path, sqlite3_errmsg(db));
                sqlite3_close(db);
                return NULL;
        }
	return db;
}


static inline void _executeSQL(T C, const char *sql) {
#if defined SQLITEUNLOCK && SQLITE_VERSION_NUMBER >= 3006012
        C->lastError = sqlite3_blocking_exec(C->db, sql, NULL, NULL, NULL);
#else
        EXEC_SQLITE(C->lastError, sqlite3_exec(C->db, sql, NULL, NULL, NULL), C->timeout);
#endif
}


static int _setProperties(T C, char **error) {
        URL_T url = Connection_getURL(delegator);
        const char **properties = URL_getParameterNames(url);
        if (properties) {
                StringBuffer_clear(C->sb);
                for (int i = 0; properties[i]; i++) {
                        if (IS(properties[i], "heap_limit")) // There is no PRAGMA for heap limit as of sqlite-3.7.0, so we make it a configurable property using "heap_limit" [kB]
                                #if defined(HAVE_SQLITE3_SOFT_HEAP_LIMIT64)
                                sqlite3_soft_heap_limit64(Str_parseInt(URL_getParameter(url, properties[i])) * 1024);
                                #elif defined(HAVE_SQLITE3_SOFT_HEAP_LIMIT)
                                sqlite3_soft_heap_limit(Str_parseInt(URL_getParameter(url, properties[i])) * 1024);
                                #else
                                DEBUG("heap_limit not supported by your sqlite3 version, please consider upgrading sqlite3\n");
                                #endif
                        else
                                StringBuffer_append(C->sb, "PRAGMA %s = %s; ", properties[i], URL_getParameter(url, properties[i]));
                }
                _executeSQL(C, StringBuffer_toString(C->sb));
                if (C->lastError != SQLITE_OK) {
                        *error = Str_cat("unable to set database pragmas -- %s", sqlite3_errmsg(C->db));
                        return false;
                }
        }
        return true;
}


/* ---------------------------------------------- ConnectionDelegate methods */


static T SQLiteConnection_new(Connection_T delegator, char **error) {
        assert(delegator);
        assert(error);
	T C;
        sqlite3 *db;
        if (! (db = _doConnect(delegator, error)))
                return NULL;
	NEW(C);
        C->db = db;
        C->delegator = delegator;
        C->timeout = SQL_DEFAULT_TIMEOUT;
        sqlite3_busy_timeout(C->db, C->timeout);
        C->sb = StringBuffer_create(STRLEN);
        if (! _setProperties(C, error))
                SQLiteConnection_free(&C);
	return C;
}


static void SQLiteConnection_free(T *C) {
	assert(C && *C);
        while (sqlite3_close((*C)->db) == SQLITE_BUSY)
               Time_usleep(10);
        StringBuffer_free(&((*C)->sb));
	FREE(*C);
}


static int SQLiteConnection_ping(T C) {
        assert(C);
        _executeSQL(C, "select 1;");
        return (C->lastError == SQLITE_OK);
}


static int SQLiteConnection_beginTransaction(T C) {
	assert(C);
        _executeSQL(C, "BEGIN TRANSACTION;");
        return (C->lastError == SQLITE_OK);
}


static int SQLiteConnection_commit(T C) {
	assert(C);
        _executeSQL(C, "COMMIT TRANSACTION;");
        return (C->lastError == SQLITE_OK);
}


static int SQLiteConnection_rollback(T C) {
	assert(C);
        _executeSQL(C, "ROLLBACK TRANSACTION;");
        return (C->lastError == SQLITE_OK);
}


static long long SQLiteConnection_lastRowId(T C) {
        assert(C);
        return sqlite3_last_insert_rowid(C->db);
}


static long long SQLiteConnection_rowsChanged(T C) {
        assert(C);
        return (long long)sqlite3_changes(C->db);
}


static int SQLiteConnection_execute(T C, const char *sql, va_list ap) {
        assert(C);
        va_list ap_copy;
        va_copy(ap_copy, ap);
        StringBuffer_vset(C->sb, sql, ap_copy);
        va_end(ap_copy);
	_executeSQL(C, StringBuffer_toString(C->sb));
	return (C->lastError == SQLITE_OK);
}


static ResultSet_T SQLiteConnection_executeQuery(T C, const char *sql, va_list ap) {
        va_list ap_copy;
        const char *tail;
	sqlite3_stmt *stmt;
	assert(C);
        va_copy(ap_copy, ap);
        StringBuffer_vset(C->sb, sql, ap_copy);
        va_end(ap_copy);
#if defined SQLITEUNLOCK && SQLITE_VERSION_NUMBER >= 3006012
        C->lastError = sqlite3_blocking_prepare_v2(C->db, StringBuffer_toString(C->sb), StringBuffer_length(C->sb), &stmt, &tail);
#elif SQLITE_VERSION_NUMBER >= 3004000
        EXEC_SQLITE(C->lastError, sqlite3_prepare_v2(C->db, StringBuffer_toString(C->sb), StringBuffer_length(C->sb), &stmt, &tail), C->timeout);
#else
        EXEC_SQLITE(C->lastError, sqlite3_prepare(C->db, StringBuffer_toString(C->sb), StringBuffer_length(C->sb), &stmt, &tail), C->timeout);
#endif
	if (C->lastError == SQLITE_OK)
		return ResultSet_new(SQLiteResultSet_new(stmt, C->maxRows, false), (Rop_T)&sqlite3rops);
	return NULL;
}


static PreparedStatement_T SQLiteConnection_prepareStatement(T C, const char *sql, va_list ap) {
        va_list ap_copy;
        const char *tail;
        sqlite3_stmt *stmt;
        assert(C);
        va_copy(ap_copy, ap);
        StringBuffer_vset(C->sb, sql, ap_copy);
        va_end(ap_copy);
#if defined SQLITEUNLOCK && SQLITE_VERSION_NUMBER >= 3006012
        C->lastError = sqlite3_blocking_prepare_v2(C->db, StringBuffer_toString(C->sb), -1, &stmt, &tail);
#elif SQLITE_VERSION_NUMBER >= 3004000
        EXEC_SQLITE(C->lastError, sqlite3_prepare_v2(C->db, StringBuffer_toString(C->sb), -1, &stmt, &tail), C->timeout);
#else
        EXEC_SQLITE(C->lastError, sqlite3_prepare(C->db, StringBuffer_toString(C->sb), -1, &stmt, &tail), C->timeout);
#endif
        if (C->lastError == SQLITE_OK) {
                int paramCount = sqlite3_bind_parameter_count(stmt);
		return PreparedStatement_new(SQLitePreparedStatement_new(C->db, stmt, C->maxRows), (Pop_T)&sqlite3pops, paramCount);
        }
	return NULL;
}


static const char *SQLiteConnection_getLastError(T C) {
	assert(C);
	return sqlite3_errmsg(C->db);
}


/* ----------------------------------------------- SQLite ConnectionDelegate */


const struct Cop_T sqlite3cops = {
        .name 		 	= "sqlite",
        .new 		 	= SQLiteConnection_new,
        .free 		 	= SQLiteConnection_free,
        .ping		 	= SQLiteConnection_ping,
        .beginTransaction	= SQLiteConnection_beginTransaction,
        .commit			= SQLiteConnection_commit,
        .rollback		= SQLiteConnection_rollback,
        .lastRowId		= SQLiteConnection_lastRowId,
        .rowsChanged		= SQLiteConnection_rowsChanged,
        .execute		= SQLiteConnection_execute,
        .executeQuery		= SQLiteConnection_executeQuery,
        .prepareStatement	= SQLiteConnection_prepareStatement,
        .getLastError		= SQLiteConnection_getLastError
};

