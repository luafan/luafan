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

* `worker: integer?`

	event worker affinity. Omitted keeps the FIFO on the main event base; an explicit non-negative worker selects that worker, and `-1` selects the main event base. Invalid or out-of-range values raise an error.

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
