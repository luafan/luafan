# luafan 单线程 event loop

luafan 在 PanPipe 中只使用一个 libevent event base、一个 DNS base 和一个 Lua runtime 线程。所有网络回调、DNS 回调、Lua coroutine 恢复、内部延迟任务和资源清理都在主 event loop 上执行。

`event_mgr_base()` 与 `event_mgr_dnsbase()` 返回唯一的主 base。历史上带有 `worker_id` 的兼容函数会忽略该参数并投递到主 base；它们不创建线程、不会创建额外 event base，也不提供 worker 配置。

Lua core 的 `lua_lock` / `lua_unlock`、Lua global lock、loop lock hand-off 和 deadlock watchdog 在此构建中都是 no-op。`FAN_RESUME` 仍负责异步事件回调恢复 coroutine，不能删除。
