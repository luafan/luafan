fan.stream
==========

### `stream_obj = stream.new(data:string?)`

stream api in c/lua to cross luajit and lua5.2+

---------
* `new()` create a new stream object for write.
* `new(data:string)` create a new stream object for read, init with data.

---------

`stream` apis (LITTLE-ENDIAN)

* `available()` get the available data length to read
* `GetU8()` read byte(1) as integer.
* `GetS24()` read byte(3) as integer.
* `GetU24()` read byte(3) as unsigned integer.
* `GetU16()` read byte(2) as unsigned integer.
* `GetU32()` read byte(4) as unsigned integer.
* `GetU30()` read byte(1-5) as unsigned integer, use 7-bit of each byte to storage integer, if the high bit is 1, that means next byte is part of this integer, return nil if buflen is not enough.
* `GetD64()` read byte(8) as double
* `GetBytes(length:number)` read byte(length) as string.
* `GetString()` read string, if buffer length does enough, return nil,expect_length.
* `AddU8(value:uinteger)` write unsigned integer as byte(1)
* `AddU16(value:uinteger)` write unsigned integer as byte(2)
* `AddS24(value:integer)` write integer as byte(3)
* `AddU24(value:uinteger)` write unsigned integer as byte(3)
* `AddU30(value:uinteger)` write unsigned integer as byte(1-5), see `GetU30`
* `AddD64(value:uinteger)` write double as byte(8)
* `AddBytes(value:string)` write string as byte(#value)
* `AddString(value:string)` write string.
* `package():string` package all data inside the write stream.
