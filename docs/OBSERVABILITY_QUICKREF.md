# 可观测性快速参考

## 配置开关总览

| 功能 | 配置项 | 默认值 | 说明 |
|------|--------|--------|------|
| **日志** | `observability.enable_logging` | `1` | 0=关闭，1=开启 |
| **日志级别** | `observability.log_level` | `"info"` | debug/info/warn/error |
| **JSON 日志** | `observability.enable_json_log` | `1` | 0=文本，1=JSON |
| **指标** | `observability.enable_metrics` | `1` | 0=关闭，1=开启 |
| **指标端口** | `observability.metrics_port` | `9090` | Prometheus 抓取端口 |
| **追踪** | `observability.enable_tracing` | `1` | 0=关闭，1=开启 |
| **采样率** | `observability.tracing_sample_rate` | `1.0` | 0.0-1.0 (100%) |

---

## 快速禁用/启用

### 完全禁用可观测性（最高性能）

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 0,
      "enable_metrics": 0,
      "enable_tracing": 0
    }
  }
}
```

### 仅启用日志（基础调试）

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 1,
      "log_level": "info",
      "enable_json_log": 0,
      "enable_metrics": 0,
      "enable_tracing": 0
    }
  }
}
```

### 生产环境推荐配置

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 1,
      "log_level": "info",
      "enable_json_log": 1,
      
      "enable_metrics": 1,
      "metrics_port": 9090,
      "metrics_path": "/metrics",
      
      "enable_tracing": 1,
      "tracing_exporter": "jaeger",
      "tracing_endpoint": "http://jaeger:14268/api/traces",
      "tracing_sample_rate": 0.1
    }
  }
}
```

---

## 常用查询

### Prometheus 查询示例

```prometheus
# QPS (每秒请求数)
rate(gateway_http_requests_total[1m])

# 错误率
rate(gateway_http_request_errors_total[1m]) / rate(gateway_http_requests_total[1m])

# P95延迟
histogram_quantile(0.95, rate(gateway_http_request_duration_seconds_bucket[1m]))

# 当前活跃请求
gateway_http_requests_in_flight

# 上游服务P99延迟
histogram_quantile(0.99, rate(gateway_http_upstream_duration_seconds_bucket[1m]))
```

### Grafana 面板配置

导入 Prometheus 数据源后，使用上述查询创建图表。

---

## 日志格式

### JSON 格式（推荐生产环境）

```json
{
  "timestamp": "2026-03-24T10:30:00.123Z",
  "level": "INFO",
  "service": "gateway",
  "event": "request_started",
  "request_id": "abc123def",
  "trace_id": "xyz789uvw",
  "message": "method=GET path=/api/users"
}
```

### 文本格式（开发调试）

```
[2026-03-24T10:30:00.123Z] [INFO] [req=abc123def] [trace=xyz789uvw] request_started: method=GET path=/api/users
```

---

## 分布式追踪头

### 接收请求（解析 traceparent）

```
traceparent: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01
```

格式：`{version}-{trace_id}-{span_id}-{flags}`
- `trace_id`: 32 字符 hex（全局唯一追踪 ID）
- `span_id`: 16 字符 hex（当前跨度 ID）
- `flags`: 01=采样，00=不采样

### 转发请求（注入 traceparent）

网关会自动生成新的 span_id 并传递给下游服务：

```
traceparent: 00-{trace_id}-{new_span_id}-{sampled}
```

---

## 故障排查命令

### 检查网关是否启动

```bash
curl http://localhost:8080/services
```

### 检查指标端点

```bash
curl http://localhost:9090/metrics
```

### 查看实时日志

```bash
tail -f logs/gateway.log
```

### Windows PowerShell 查看日志

```powershell
Get-Content logs\gateway.log -Wait -Tail 50
```

---

## 性能调优参数

### 高并发场景（降低开销）

```json
{
  "observability": {
    "log_level": "warn",          // 仅记录警告和错误
    "tracing_sample_rate": 0.01   // 1% 采样
  }
}
```

### 调试模式（详细日志）

```json
{
  "observability": {
    "log_level": "debug",         // 所有调试信息
    "tracing_sample_rate": 1.0    // 100% 采样
  }
}
```

### 平衡模式（生产推荐）

```json
{
  "observability": {
    "log_level": "info",          // 一般信息
    "tracing_sample_rate": 0.1    // 10% 采样
  }
}
```

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `src/logger.c` | 日志模块实现 |
| `src/metrics.c` | Prometheus 指标实现 |
| `src/tracer.c` | 分布式追踪实现 |
| `include/gateway.h` | 数据结构和函数声明 |
| `config/gateway_config.json` | 配置文件 |
| `docs/OBSERVABILITY_CONFIG.md` | 详细配置文档 |
| `test_observability.bat` | 测试脚本 |

---

## 下一步

1. ✅ 修改 `config/gateway_config.json` 启用功能
2. ✅ 运行 `bin\c_gateway.exe config\gateway_config.json`
3. ✅ 访问 `http://localhost:9090/metrics` 查看指标
4. ✅ 查看 `logs/gateway.log` 查看日志
5. 📊 配置 Prometheus + Grafana 监控面板
