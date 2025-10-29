#include "luamariadb_stmt_execute.h"

static int stmt_execute_result(lua_State *L, STMT_CTX *st)
{
  MYSQL_RES *prepare_meta_result = mysql_stmt_result_metadata(st->my_stmt);

  if (!prepare_meta_result)
  {
    my_ulonglong affected_rows = mysql_stmt_affected_rows(st->my_stmt);
    lua_pushinteger(L, affected_rows);
    return 1;
  }
  else
  {
    unsigned int field_count = mysql_num_fields(prepare_meta_result);
    MYSQL_FIELD *fields = mysql_fetch_fields(prepare_meta_result);
    lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);
    int tableidx = lua_gettop(L);

    MYSQL_BIND *rbind = get_or_create_ud(L, tableidx, &st->rbind,
                                         field_count * sizeof(MYSQL_BIND));
    int *buffers =
        get_or_create_ud(L, tableidx, &st->buffers, field_count * sizeof(int));
    unsigned long *bufferlens = get_or_create_ud(
        L, tableidx, &st->bufferlens, field_count * sizeof(unsigned long));
    my_bool *is_nulls = get_or_create_ud(L, tableidx, &st->is_nulls,
                                         field_count * sizeof(my_bool));

    mysql_free_result(prepare_meta_result);

    int i = 0;
    for (; i < field_count; i++)
    {
      MYSQL_FIELD field = fields[i];

      switch (field.type)
      {
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      {
        // void *buffer = get_or_create_ud(L, tableidx, &buffers[i], 1024);
        // rbind[i].buffer_type = MYSQL_TYPE_MEDIUM_BLOB;
        // rbind[i].length = &bufferlens[i];
        // rbind[i].is_null = &is_nulls[i];
        // rbind[i].buffer = buffer;
        // rbind[i].buffer_length = 1024;
      }

      // printf("field.length = %d\n", field.length);
      // break;
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
      {
        void *buffer = get_or_create_ud(L, tableidx, &buffers[i], field.length);
        MYSQL_SET_VARSTRING(&rbind[i], buffer, field.length);
      }
      break;
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_TINY:
      {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(long long));
        MYSQL_SET_LONGLONG(&rbind[i], buffer);
      }
      break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(double));
        MYSQL_SET_DOUBLE(&rbind[i], buffer);
      }
      break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP:
      {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(MYSQL_TIME));
        MYSQL_SET_TIMESTAMP(&rbind[i], buffer);
      }
      break;

      default:
        printf("unknown field.type: %d\n", field.type);
        break;
      }

      rbind[i].length = &bufferlens[i];
      rbind[i].is_null = &is_nulls[i];
    }

    lua_pop(L, 1); // pop table

    if (mysql_stmt_bind_result(st->my_stmt, rbind))
    {
      return luamariadb_push_stmt_error(L, st);
    }

    lua_pushboolean(L, 1);
    return 1;
  }
}

static void stmt_execute_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  STMT_CTX *st = (STMT_CTX *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  }
  else
  {
    int ret = 0;
    int status = mysql_stmt_execute_cont(&ret, st->my_stmt, bag->status);
    if (status)
    {
      wait_for_status(L, st->ctx, st, status, stmt_execute_cont,
                      bag->extra);
    }
    else if (ret == 0)
    {
      int count = stmt_execute_result(L, st);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(st);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }
  event_free(bag->event);
  free(bag);
}

LUA_API int stmt_execute_start(lua_State *L)
{
  STMT_CTX *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_execute_start(&ret, st->my_stmt);

  if (status)
  {
    REF_CO(st);
    wait_for_status(L, st->ctx, st, status, stmt_execute_cont, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    int count = stmt_execute_result(L, st);
    if (count >= 0)
    {
      return count;
    }
    else
    {
      REF_CO(st);
      return lua_yield(L, 0);
    }
  }
  else
  {
    return luamariadb_push_stmt_error(L, st);
  }
}
