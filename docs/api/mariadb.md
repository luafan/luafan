fan.mariadb
===========

### Const
* `mariadb.LONG_DATA` used as placeholder for `stmt:bind_param`

---------

### `conn = connect(dbname:string, username:string, password:string, host:string?, port:integer?)`

connect to mariadb server.

---------
 `conn` apis

* `close()` close connecion
* `ping()` mariadb ping
* `escape()` escape string as mariadb format.
* `execute(sql:string)` execute a sql script, return [cursor](#cursor)
* `setcharset(charset:string)` set connection charset.
* `prepare(sql:string)` prepare a sql [prepared statement](#preparedstatement).
* `commit()` available for innodb only.
* `rollback()` available for innodb only.
* `autocommit()` available for innodb only.
* `getlastautoid()` get the last autoincrement id created by this connection.

---------

PreparedStatement
=================

### `close()`
close statement.
### `bind_param(...)`
bind statement parameters.
### `send_long_data(idx, value:string)`
if parameter n of `bind_param` (n start from 0) is `mariadb.LONG_DATA`, use `stmt: send_long_data(n, value:string)` to send the long data to n.
### `execute()`
execute this statement
### `fetch()`
get the result row, return `success:boolean, field1, field2, ...`

Cursor
======

### `close()`
close cursor.

### `getcolnames()`
return the list of column names.

### `getcoltypes()`
return the list of column types.

### `fetch()`
get the result row, return table, format as `{columnkey = value, ...}`

### `numrows()`
get the row count.
