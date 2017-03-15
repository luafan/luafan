fan.objectbuf
=============

objectbuf is used to serialize lua object to string, and deserialize string to lua object. 

## lua object type supported
`table`, `string`, `number`, `boolean`

## why not cjson
* cjson does not support lua table mixed with array part and hash part.
* cjson does not support complex nesting table that with children reference it self. (`a = {children = {}}; b = {parent = a}; a.children[1] = b;`, encode `a` or `b`)
* json can't detect duplicate field (`a = {}; for i=1,10000 do a[i] = "duplicate string" end`, encode `a`, objectbuf got 59,773 bytes output, cjson got 190,001 bytes output.)

## Notice
objectbuf may a little slow than cjson on small lua object, but it is faster than cjson on huge lua object.

## encode format
`flag (int8)` describe whether stream contains number/integer/string/table section, if there is no number/integer/string/table section, the lowest bit of flag is used to describe boolean value 1 => true, 0 => false.

Each section start with a u30([fan.stream](stream.md)) value to describe the count of number/integer/string/table inside the section.

there is a builtin index dictionary:

* 0,1 is preserved by false and true

* if it contains number section, the number index start from 2, each number use 8-bytes space.

* if it contains integer section, the integer index start from 2 + #(numbers), each integer use 1-5 bytes(u30) space.

* if it contains string section, the string index start from 2 + #(numbers) + #(integers), the string format is the same as [fan.stream](stream.md), length(u30)+buffer(byte[length])

* if it contains table section, the string index start from 2 + #(numbers) + #(integers) + #(strings), each table is encoded as string with key(u30)-value(u30) pairs.

* The first item of the last section will be the encode/decode lua object, the selection order is table => string => integer => number.

APIs
====

### `sym = objectbuf.symbol(sample:table)`
build symbol table for object to encode/decode to reduce output data size.

### `data = objectbuf.encode(obj:object, sym?)`
encode lua object to string.

### `obj = objectbuf.decode(data:string, sym?)`
decode lua object from string.
