fan.stream
==========

### `stream_obj = stream.new(data:string?)`

stream api in c/lua to cross luajit and lua5.2+

---------
* `new()` create a new stream object for write.
* `new(data:string)` create a new stream object for read, init with data.

---------

`stream` apis (LITTLE-ENDIAN)

* `available():uinteger` get the available data length to read
* `GetU8():uinteger` read byte(1) as integer.
* `GetS24():integer` read byte(3) as integer.
* `GetU24():uinteger` read byte(3) as unsigned integer.
* `GetU16():uinteger` read byte(2) as unsigned integer.
* `GetU32():uinteger` read byte(4) as unsigned integer.
* `GetU30():uinteger` read byte(1-5) as unsigned integer, use 7-bit of each byte to storage integer, if the high bit is 1, that means next byte is part of this integer, return nil if buflen is not enough.
* `GetD64():number` read byte(8) as double
* `GetBytes(length:number):string` read byte(length) as string.
* `GetString():string` read string, if buffer length does enough, return nil,expect_length.
* `AddU8(value:uinteger)` write unsigned integer as byte(1)
* `AddU16(value:uinteger)` write unsigned integer as byte(2)
* `AddS24(value:integer)` write integer as byte(3)
* `AddU24(value:uinteger)` write unsigned integer as byte(3)
* `AddU30(value:uinteger)` write unsigned integer as byte(1-5), see `GetU30`
* `AddD64(value:number)` write double as byte(8)
* `AddBytes(value:string)` write string as byte(#value)
* `AddString(value:string)` write string.
* `package():string` package all data inside the write stream.
* `prepare_add()` prepare read stream for append.
* `prepare_get()` prepare write stream for read.
* `mark()` mark stream read offset, will be cleaned after prepare_add.
* `reset()` reset stream read offset to last marked position.
