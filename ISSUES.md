# Issues — 问题跟踪

> 持续维护的问题清单，记录已知问题、修复状态与新发现的问题。
> 本文档由 2026-08-13 的一次性代码审查报告 `CODE_REVIEW.md` 转化而来（已重命名）。

最后更新：2026-09-05

## 状态总览

| 分类 | 总数 | 已解决 | 待解决 |
|------|------|--------|--------|
| Bug | 15 | 10 | 5 |
| 安全 | 12 | 3 | 9 |
| 架构 | 9 | 5 | 4 |
| 新发现 | 6 | 3 | 3 |
| **合计** | **42** | **21** | **21** |

---

## 事件复盘：QQ 审批失效与幽灵端口（2026-08-25）

**现象**：8-20 起所有 MCP 命令不经审批直接执行；当晚多次重启服务后，审批消息仍无法送达用户 QQ，且部分请求无任何响应、无任何日志。

**时间线**：
1. 8-20 白天公司带宽故障，QQ WebSocket 反复重连超时（正常现象）。
2. 8-20 15:22 服务随开机启动，DNS 尚未就绪 → `Could not resolve hostname` → approver 启动失败。`create_approver` 失败后返回 nullptr 且**永不重试**，此后 5 天所有命令跳过审批直通。
3. 8-25 多次重启服务，但旧实例停机时卡死在内核态（线程困于 WinHTTP 调用），成为不可见进程继续持有 12380 监听 socket；配合 `SO_REUSEADDR`，新实例静默双绑定成功，新连接随机落入死实例的 backlog —— 表现为请求被吞、零日志。

**修复**（同日）：见下方 BUG-M7 / BUG-M8 / BUG-M9。另将审批关键路径补上 info 日志（进入审批门、POST 响应体、批准/拒绝/超时结果），`flush_on` 改为 debug 级全量刷盘以便诊断。

**遗留**：卡死在内核态的幽灵进程只能通过重启系统回收（`taskkill /F` 无效）。重启后恢复标准端口即可。

---

## 已解决（2026-08-13）

### 架构

- [x] **ARCH-1** Approver 无统一抽象接口 — 新建 `service/src/approver.h`，`TelegramApprover`/`QQApprover` 实现该接口，`tcp_server` 只依赖抽象类型。
- [x] **ARCH-2** `max_connections` 配置名误导 — 改名 `listen_backlog`（仅作 accept backlog，保留旧名弃用别名兼容），新增 `max_concurrent_clients` 真正限制并发线程数。
- [x] **ARCH-4** `legally.h` 死代码 — 已删除。
- [x] **ARCH-7** 优雅停机不完整 — 连接线程用 `vector<std::thread>` + `done` 标志跟踪并 `join()`；`CommandExecutor` 维护子进程 handle 列表、停机 `terminate_all()`。

### Bug

- [x] **BUG-C1** config 解析崩溃 — 见下方「审查错误更正」，已用显式范围检查修复。
- [x] **BUG-H1** detached 线程 use-after-free — approver/executor 改 `shared_ptr` 持有 + 线程 `join()`。
- [x] **BUG-H2** 无限并发线程（DoS）— 被 **ARCH-2** 的 `max_concurrent_clients` 顺带解决（超限拒绝连接）。
- [x] **BUG-M1** `TelegramApprover::offset_` 跨线程无锁 — 改 `std::atomic<long long>` + CAS 单调更新。
- [x] **BUG-M2** QQ 重连旧心跳线程与 handle 关闭竞态 — 调整为先 `join()` 旧心跳线程再 `disconnect`。
- [x] **BUG-M3** `tcp_client.py` `timeout_seconds=0` 与文档不符 — `0` 时 socket 改为阻塞（`None`）。
- [x] **BUG-L2** `GetExitCodeProcess` 返回值未检查 — 失败时 `exit_code = -1`。
- [x] **BUG-L3** `send_all` 无发送超时 — 补 `SO_SNDTIMEO`。
- [x] **BUG-L6** `g_stop_event` 创建后从未等待 — 移除死代码。

### 安全

- [x] **SEC-C2** 全局禁用 SSL 证书校验 — 新增 `ssl_verify` 配置（默认 `true`），libcurl 校验默认开启、可显式关闭。
- [x] **SEC-L2** Debug 日志记录敏感命令 — 新增 `log_level` 配置（默认 `info`）。
- [x] **SEC-L3** 无执行审计日志 — `execute_command` 入口/出口写 `AUDIT:` 日志（命令、exit_code、耗时）。

---

## 审查错误更正

- **BUG-C1 机制判断有误**：原报告认为 `j["port"].get<uint16_t>()` 在值越界（如 `99999`）时抛 `out_of_range` 导致启动崩溃。**实测 nlohmann 3.12 的行为是 static_cast 静默截断**（`99999` → `34463`，源码 `detail/conversions/from_json.hpp` 直接 `static_cast`），**不崩溃**，而是静默绑定到错误端口。已改用显式范围检查（`get_uint<T>` + `numeric_limits<T>::max()`）正确修复：越界 warn 并回退默认值。

---

## 新发现的问题（2026-08-25 事件）

- [x] **BUG-M7** approver 启动失败后永不重试 — `create_approver` 失败即返回 nullptr，审批在整个服务生命周期内静默失效。已加后台重试线程（15s 间隔，成功后经 `atomic<shared_ptr>` 原子接入），服务监听不受重试影响。
- [x] **BUG-M8** `SO_REUSEADDR` 导致幽灵双绑定 — 前一实例停机卡死未释放端口时，新实例仍能绑定成功，连接随机落入无人 accept 的 backlog（请求被吞、零日志）。改为 `SO_EXCLUSIVEADDRUSE`，双绑定时启动报错。
- [x] **BUG-M9** 停机无限 join 卡死产生幽灵进程 — `QQApprover::stop()` 无条件 join 可能永久阻塞在内核 WinHTTP 调用里的线程；`TcpServer::join_all_threads` 同样无界。改为有界等待（receive 10s / heartbeat 4s / 客户端线程共 15s），超时 detach 并跳过 handle 清理，由进程退出回收；后台线程增加 `*_exited_` 标志供 stop 判定。
- [ ] **审计日志命令未转义**：`AUDIT: ... command="{}"` 直接拼接命令，命令含 `"` 或换行会破坏日志格式、可伪造审计记录。建议对命令转义/限制长度。
- [ ] **`command_executor.cpp` handle 泄漏（历史遗留）**：`CreatePipe` 部分失败时成功的那组 read/write handle 未关闭；`NUL` 打开失败时只关 read ends、write ends 泄漏。建议用 RAII handle 封装。
- [ ] **`terminate_all` 不等待进程退出**：`TerminateProcess` 后未 `WaitForSingleObject`，服务退出瞬间子进程可能短暂残留（系统随后回收）。低风险。

---

## 待解决（原始报告遗留，本次范围外）

### Bug

- [ ] **BUG-H3** 命令输出累积阶段无大小上限（仅序列化时截断）→ 内存耗尽 DoS。
- [ ] **BUG-M4** `tcp_client.py` 把读超时误报为「无法连接」。
- [x] **BUG-M5** QQ 按钮 `permission.type=2` 缺 `specify_user_ids` — **实测非问题（2026-08-25）**：C2C 单聊场景下该格式消息正常送达、按钮回调 `INTERACTION_CREATE` 正常返回，审批全链路验证通过。原审查结论不适用于当前使用方式。
- [ ] **BUG-L1** `main.cpp` 无参数时静默尝试服务模式，返回码应为非 0。
- [ ] **BUG-L4** QQ `on_interaction_create` 对所有点击都 ack，浪费配额。
 - [ ] **BUG-L5** QQ Markdown 未转义命令中的 `*`/`_`/`` ` ``。（2026-09-05 新增 `description` 字段同样进入 QQ 审批消息，未转义的影响范围随之扩大。）

### 安全

- [ ] **SEC-C1** TCP 命令通道无认证（本地任意进程可执行 SYSTEM 命令）— **最紧急**。
- [ ] **SEC-H1** 服务以 LocalSystem 运行、无权限隔离（建议 Job Object / 专用账户降权）。
- [ ] **SEC-M1** 无命令白名单/黑名单。
- [ ] **SEC-M2** `working_dir` 不校验 UNC 路径（NTLM 哈希泄露）。
- [ ] **SEC-M3** Telegram `chat_id` 校验不足（群成员可批准）。
- [ ] **SEC-M4** 无速率限制。
- [ ] **SEC-L1** `config.json` 落盘明文存储真实凭证（建议 DPAPI / 环境变量）。
- [ ] **SEC-L4** `html_escape` 不处理空字节。
- [ ] **SEC-L5** 协议无版本号字段。

### 架构

- [ ] **ARCH-3** C++ 服务零单元测试（`parse_request`/`serialize_response`/`config.cpp` 等）。
- [ ] **ARCH-5** `admin-test.py` 重复实现协议逻辑，未复用 `tcp_client`。
- [ ] **ARCH-6** `service.cpp` 全局可变状态过多。
- [x] **ARCH-8** 日志 `flush_on` 刷盘频繁影响性能 — 本服务日志量极小（每次命令几行），性能影响可忽略；2026-08-25 排障时 `flush_on(info)` 反而扣住 debug 日志误导诊断，故改为 debug 级全量即时刷盘， durability 优先。
- [ ] **ARCH-9** 无 CI/CD。
