# MySQL 预处理语句并发 use-after-free 修复记录

- 服务：`vr_question_service`（基于 SDK 的 `q_dbp` 连接池 + MySQL 预处理语句缓存）
- 现象：并发请求下进程崩溃，ASan 报 `heap-use-after-free`；单并发不复现
- 状态：**已修复并验证**（ASan 高并发压测 0 错误，生产构建正常运行）

---

## 1. 现象

通过网关 `GET /api/vr-questions` 做压测时，服务进程在高并发下崩溃。
用 `ab -c 50` 即可稳定复现，单发（`-c 1`）与低并发（`-c 4`）基本不触发。

最初 ASan 报告指向 MySQL 驱动取行函数：

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x... at pc ... in memcpy
    #0 memcpy ...
    #1 mysql_stmt_fetch            (libmariadb)
    #5 my_res_row  src/db/driver/mysql/q_db_mysql.c:161
    #6 q_result_next src/db/db.c:497
    #7 q_ctx_query   src/mapper/mapper.c:647
```

表面看是：缓存的 `MYSQL_STMT*` 绑定的 `out_buf` 已被释放，下一请求复用该语句执行 `mysql_stmt_fetch` 时再次写入已释放内存。

---

## 2. 排查与修复过程（含一次弯路）

### 2.1 第一轮修复：解绑 + 拷出（未根治）

初步假设：语句被连接池按连接缓存、跨请求复用，而 `out_buf` 随 `my_result_t` 释放，
缓存语句仍持有对 `out_buf` 的悬空绑定指针，下次绑定前先 fetch 即踩内存。

于是实施两处：

- **Fix A（核心）**：`my_res_free` 释放 `out_buf` 前，对缓存语句调用
  `mysql_stmt_bind_result(stmt, NULL)` 解绑，消除悬空指针。
- **Fix B（加固）**：`my_res_row` 不再把 `vals[i].str` 直接别名到 `out_buf`，
  而是拷到 `my_result_t` 自有的 `row_copy` 缓冲（随结果释放）。

ASan 重测后，原崩溃位移，暴露出**新的 heap-use-after-free**：

```
READ of size 5 ... in json_stringn (libjansson)
    #3 q_ctx_query mapper.c:658
    #5 work_cb    src/async.c:18
freed by thread T4:
    #1 ensure_row_copy  q_db_mysql.c:106   (realloc 移动缓冲)
    #2 my_res_row       q_db_mysql.c:219
    #3 q_result_next    db.c:497
    #4 q_ctx_query      mapper.c:647
```

**根因分析**：Fix B 有 bug——`my_res_row` 逐列把字符串拷进 `row_copy`，
但 `ensure_row_copy` 在列循环内部 `realloc` 移动了缓冲；一旦移动，
**同一行里先设好的 `vals[i].str` 仍指向已被释放的旧块**，随后 `json_stringn` 读到它 → UAF。

**结论**：Fix B 引入的 realloc 会使同行内指针悬空，且原始别名 `out_buf` 在单请求内本就安全
（循环内消费、`my_res_free` 后才释放）。Fix B **不必要且有害**，已整体回退，仅保留 Fix A。

### 2.2 第二轮修复：连接持有到结果消费完（真正根因）

回退 B 后 ASan 仍报错，且这次 "freed by" 栈来自**另一条代码路径**：

```
READ of size ... in mysql_stmt_fetch (libmariadb)    my_res_row q_db_mysql.c:167
freed by thread T4:
    #1 free (libmariadb)
    #5 exec_prepared  mapper.c:591   ->  q_stmt_query db.c:615
```

即：一侧在 `my_res_row` 里对某 `MYSQL_STMT*` 做 `mysql_stmt_fetch`，
另一侧在 `exec_prepared` 的 `q_stmt_query`（`mysql_stmt_store_result`）里
**释放并重分配了同一个 `MYSQL_STMT*` 的内部结果缓冲**。

这意味着：**同一个缓存预处理语句（`= 同一条连接`）同时被两个并发请求驱动**。

追查连接生命周期（`exec_prepared`, `mapper.c:542`）：

```c
conn = q_dbp_get(c->pool, ...);          // 取连接
st   = stmt_cached(conn, sql);           // 命中连接上缓存的预处理语句
rc   = q_stmt_query(st, &res, ...);      // mysql_stmt_store_result 在 ms->stmt 上分配结果缓冲
...
q_dbp_put(conn);                         // 第 609 行：返回 res 之前就把连接还池  <-- 问题点
*out = res;                              // 把结果交回调用方
```

调用方 `q_ctx_query` **之后**才在 `q_result_next` 循环里对该连接的缓存语句 `ms->stmt`
做 `mysql_stmt_fetch`。连接还池后，另一线程拿到同一连接、命中同一缓存语句、
`mysql_stmt_store_result` 释放其缓冲 —— 正在 fetch 的线程踩到已释放内存。

**真正的根因**：连接结果集被消费完（`q_result_free`）之前就把连接还池，
导致缓存的预处理语句处于“被并发请求重驱动”的不安全窗口。

---

## 3. 最终修复

### 3.1 连接随结果归还（治本）

让连接保持独占，直到结果被消费完：

- `sdk/include/db.h`：`struct q_result` 增加 `q_conn_t *owner` 字段；新增
  `void q_result_set_owner(q_result_t *r, q_conn_t *conn);`
- `sdk/src/db/db.c`：
  - 新增 `q_result_set_owner`；
  - `q_result_free` 在释放结果后调用 `q_dbp_put(r->owner)` 归还连接。
- `sdk/src/mapper/mapper.c`：`exec_prepared` 成功路径改为
  `q_result_set_owner(res, conn)`，删除早退的 `q_dbp_put(conn)`。

效果：取行期间连接独占，缓存语句不会被其他请求重驱动。
所有结果消费者（`q_ctx_query`、`q_ctx_query_struct`、`q_ctx_exec`）均通过
`q_result_free` 释放结果，连接得以正确归还，无泄漏。

### 3.2 解绑（纵深防御，保留 Fix A）

`sdk/src/db/driver/mysql/q_db_mysql.c` 的 `my_res_free`：
释放 `out_buf` 前对缓存语句 `mysql_stmt_bind_result(stmt, NULL)`，
即使未来仍存在其他复用窗口，也消除了悬空绑定指针。

---

## 4. 修改文件清单

| 文件 | 改动 |
| --- | --- |
| `sdk/include/db.h` | `struct q_result` 增加 `owner`；声明 `q_result_set_owner` |
| `sdk/src/db/db.c` | 实现 `q_result_set_owner`；`q_result_free` 归还 `owner` 连接 |
| `sdk/src/mapper/mapper.c` | `exec_prepared` 成功路径挂 `owner`，不再提前还池 |
| `sdk/src/db/driver/mysql/q_db_mysql.c` | `my_res_free` 解绑结果缓冲（Fix A，纵深防御） |

---

## 5. 验证

### 5.1 ASan 并发压测（复现条件最大化）

- 构建：SDK + 服务均加 `-fsanitize=address`，`threads=4`、关闭网关注册、直连 8099。
- 压测：`ab -n 10000 -c 64` → **0 个失败，进程存活，无任何 `ERROR: AddressSanitizer`**。
- 此前同一条件必崩，确认 UAF 已消除。

### 5.2 生产构建性能基线（经网关 8080）

- 构建：不带 ASan，`-O2`，`threads=4`，已注册网关。
- 单发：`GET /api/vr-questions` → `200`。
- 压测 `ab -n 10000 -c 50`：
  - **Requests per second ≈ 1366**
  - **Failed requests = 0**
  - **99% 延迟 ≈ 58 ms**

---

## 6. 备注

- 连接池本身（`q_dbp_get`/`q_dbp_put`）的节点独占性是正确的；本问题不是池把同一条连接
  同时交给两个线程，而是**同一连接被提前还池后被另一条线程复用，并重新驱动其缓存语句**。
- 修复后连接在取行期间独占，最大并发占用连接数 = 并发请求数，受 `pool.max_open` 约束；
  连接不足时 `q_dbp_get` 优雅返回“get connection failed”，不再崩溃。
- Fix B（row_copy 拷出）因 realloc 致同行内指针悬空已被移除；当前实现
  `vals[i].str` 别名 `out_buf` 在单请求内安全（循环内消费、`my_res_free` 后才释放）。
