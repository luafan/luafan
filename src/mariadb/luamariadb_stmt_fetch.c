static int stmt_fetch_result(lua_State *L, st_data *st) {
  MYSQL_RES *prepare_meta_result = mysql_stmt_result_metadata(st->my_stmt);

  unsigned int field_count = mysql_num_fields(prepare_meta_result);
  MYSQL_FIELD *fields = mysql_fetch_fields(prepare_meta_result);

  mysql_free_result(prepare_meta_result);

  lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);
  int tableidx = lua_gettop(L); // never pop

  int *buffers =
      get_or_create_ud(L, tableidx, &st->buffers, field_count * sizeof(int));
  unsigned long *bufferlens = get_or_create_ud(
      L, tableidx, &st->bufferlens, field_count * sizeof(unsigned long));
  my_bool *is_nulls = get_or_create_ud(L, tableidx, &st->is_nulls,
                                       field_count * sizeof(my_bool));

  lua_newtable(L);

  int i = 0;
  for (; i < field_count; i++) {
    MYSQL_FIELD field = fields[i];

    if (is_nulls[i]) {
      lua_pushnil(L);
    } else {
      switch (field.type) {
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:

      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING: {
        void *buffer = get_or_create_ud(L, tableidx, &buffers[i], field.length);
        lua_pushlstring(L, buffer, bufferlens[i]);
      } break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_TINY: {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(double));
        lua_pushnumber(L, *((double *)buffer));
      } break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP: {
        MYSQL_TIME *buffer = (MYSQL_TIME *)get_or_create_ud(
            L, tableidx, &buffers[i], sizeof(MYSQL_TIME));

        if (buffer->time_type > 0) {
          lua_newtable(L);

          lua_pushinteger(L, buffer->year);
          lua_setfield(L, -2, "year");

          lua_pushinteger(L, buffer->month);
          lua_setfield(L, -2, "month");

          lua_pushinteger(L, buffer->day);
          lua_setfield(L, -2, "day");

          lua_pushinteger(L, buffer->hour);
          lua_setfield(L, -2, "hour");

          lua_pushinteger(L, buffer->minute);
          lua_setfield(L, -2, "minute");

          lua_pushinteger(L, buffer->second);
          lua_setfield(L, -2, "second");

          lua_pushinteger(L, buffer->second_part);
          lua_setfield(L, -2, "second_part");
        } else {
          lua_pushnil(L);
        }
      } break;

      default:
        lua_pushnil(L);
        break;
      }
    }

    lua_setfield(L, -2, field.name);
  }

  return 1;
}

static void stmt_fetch_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  } else {
    int status = mysql_stmt_fetch_cont(&ret, st->my_stmt, ms->status);
    if (status) {
      wait_for_status(L, st->conn_data, st, status, stmt_fetch_cont, ms->extra);
    } else if (ret == 0) {
      int count = stmt_fetch_result(L, st);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(st);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }
  event_free(ms->event);
  free(ms);
}

LUA_API int stmt_fetch_start(lua_State *L) {
  st_data *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_fetch_start(&ret, st->my_stmt);
  if (status) {
    REF_CO(st);
    wait_for_status(L, st->conn_data, st, status, stmt_fetch_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    int count = stmt_fetch_result(L, st);
    return count;
  } else if (ret == MYSQL_NO_DATA) {
    return 0;
  } else {
    return luamariadb_push_stmt_error(L, st);
  }
}
