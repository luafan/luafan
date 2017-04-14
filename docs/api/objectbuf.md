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

Benchmark
=========

* result

| LUA Version | Name                | Time Cost (sec) |
| ----------- | ------------------- |:---------------:|
| luajit      | objectbuf           | 2.4335000514984 |
| luajit      | objectbuf + sym     | 0.5157539844512 |
| luajit      | cjson               | 8.1667900085449 |

| LUA Version | Name                | Time Cost (sec) |
| ----------- | ------------------- |:---------------:|
| lua5.3      | objectbuf           | 4.0057001113892 |
| lua5.3      | objectbuf + sym     | 0.9010729789733 |
| lua5.3      | cjson               | 9.2845618724823 |

* code

```lua
require "compat53"

local objectbuf = require "fan.objectbuf"
local fan = require "fan"
local utils = require "fan.utils"
local cjson = require "cjson"

local a = {
    b = {
        1234556789,
        12345.6789,
        nil,
        -1234556789,
        -12345.6789,
        0,
        "asdfa",
        d = {
            e = f
        }
    },
    averyvery = "long long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long textlong long text",
}

-- cjson does not support this.
-- a.b.a = a

-- set same random seed for benchmark.
math.randomseed(0)

for i=1,100 do
    table.insert(a.b, string.rep("abc", math.random(1000)))
end

local sym = objectbuf.symbol({ b = {a = ""}, averyvery = ""})

local loopcount = 10000

local data = objectbuf.encode(a)
local data_sym = objectbuf.encode(a, sym)
local data_json = cjson.encode(a)

local start = utils.gettime()
for i=1,loopcount do
    objectbuf.encode(a)
end
print("objectbuf.encode", utils.gettime() - start)

local start2 = utils.gettime()
for i=1,loopcount do
    objectbuf.decode(data)
end
print("objectbuf.decode", utils.gettime() - start2)
print("objectbuf\n", utils.gettime() - start)

local start = utils.gettime()
for i=1,loopcount do
    objectbuf.encode(a, sym)
end
print("objectbuf.encode_sym", utils.gettime() - start)

local start2 = utils.gettime()
for i=1,loopcount do
    objectbuf.decode(data_sym, sym)
end
print("objectbuf.decode_sym", utils.gettime() - start2)
print("objectbuf.sym\n", utils.gettime() - start)

local start = utils.gettime()
for i=1,loopcount do
    cjson.encode(a)
end
print("cjson.encode", utils.gettime() - start)

local start2 = utils.gettime()
for i=1,loopcount do
    cjson.decode(data_json)
end
print("cjson.decode", utils.gettime() - start2)
print("cjson\n", utils.gettime() - start)

os.exit()
```
