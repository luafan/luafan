fan.fifo
========

### `conn = fifo.connect(arg:table)`

connect to fifo file, create a new one if not exist, [Samples](#fifo-sample)

---------
keys in the `arg`:

* `name: string`

	fifo file name/path.

* `mode: integer`

	fifo file permission, default 0600.

* `rwmode: string`

	fifo read/write mode, can be "r" "w" "rw", default "r"

* `onread: function`

	stream input callback, available if `rwmode` is "r", arg1 => buffer_in:string

* `onsendready: function`

	stream output complete callback, available if `rwmode` is "w", no arg

* `ondisconnected: function`

	reader disconnected callback, available if `rwmode` is "w", arg1 => reason:string

---------
conn apis:

### `send_req()`
request to send data, if possiable, `onsendready` will callback.

### `send(data:string)`
do send data inside `onsendready`
