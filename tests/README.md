# luafan 测试

luafan 现在使用单一 libevent event base、单一 DNS base 和单一 Lua runtime 线程。测试矩阵覆盖普通 Lua/C 模块、HTTP、TCP、UDP、FIFO、MariaDB、生命周期和内存清理行为。

## 构建

使用仓库的 `CMakeLists.txt` 构建 `fan` 模块和 `run_c_tests`。构建不再提供 worker pool、worker event base、Lua worker lock 或 hooked worker interpreter 选项。

## 运行

- `./run_all_tests.sh --c-only --verbose`：C 单元测试
- `./run_all_tests.sh --lua-only --verbose`：Lua 测试
- `./run_all_tests.sh --leak-check`：ASan/LSan 生命周期检查

worker/owner affinity、cross-worker combinations、worker lock granularity 和 worker-specific MariaDB 测试已删除，因为这些能力不再存在。`fan.worker` 若仍被测试，指的是独立进程池模块，不属于 event loop worker。
