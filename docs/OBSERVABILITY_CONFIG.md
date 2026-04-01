# 可观测性配置指南

## 概述

微服务网关现已集成完整的可观测性功能，包括：
- ✅ **结构化日志** (Logging)
- ✅ **Prometheus 指标** (Metrics)  
- ✅ **分布式追踪** (Tracing)

所有功能均可通过配置文件灵活控制。

---

## 配置参数说明

### 1. 日志配置 (Logging)

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 1,        // 总开关：1=启用，0=禁用
      "log_level": "info",        // 日志级别：debug/info/warn/error
      "enable_json_log": 1        // JSON 格式：1=JSON, 0=文本
    }
  }
}
```

**参数详解：**
- `enable_logging`: 完全关闭或开启日志系统
- `log_level`: 
  - `debug`: 调试信息（最详细）
  - `info`: 一般信息（推荐生产环境）
  - `warn`: 警告及以上
  - `error`: 仅错误信息
- `enable_json_log`: 
  - `1`: 输出 JSON 格式，便于 ELK/Splunk 等工具解析
  - `0`: 输出文本格式，便于人工阅读

**示例输出：**

JSON 格式：
```json
{"timestamp":"2026-03-24T10:30:00.123Z","level":"INFO","service":"gateway","event":"request_started","request_id":"abc123","trace_id":"xyz789","message":"method=GET path=/api/users"}
```

文本格式：
```
[2026-03-24T10:30:00.123Z] [INFO] [req=abc123] [trace=xyz789] request_started: method=GET path=/api/users
```

---

### 2. Prometheus 指标 (Metrics)

```json
{
  "gateway": {
    "observability": {
      "enable_metrics": 1,          // 指标收集开关
      "metrics_port": 9090,         // Prometheus 抓取端口
      "metrics_path": "/metrics"    // 指标暴露路径
    }
  }
}
```

**参数详解：**
- `enable_metrics`: 关闭或开启指标收集
- `metrics_port`: Prometheus Server 从这里抓取数据
- `metrics_path`: 指标端点路径

**暴露的指标：**

```prometheus
# HELP gateway_http_requests_total Total number of HTTP requests
# TYPE gateway_http_requests_total counter
gateway_http_requests_total 1523

# HELP gateway_http_request_errors_total Total number of failed HTTP requests
# TYPE gateway_http_request_errors_total counter
gateway_http_request_errors_total 3

# HELP gateway_http_requests_in_flight Current number of requests being processed
# TYPE gateway_http_requests_in_flight gauge
gateway_http_requests_in_flight 12

# HELP gateway_http_request_duration_seconds HTTP request latency in seconds
# TYPE gateway_http_request_duration_seconds histogram
gateway_http_request_duration_seconds_bucket{le="0.010"} 856
gateway_http_request_duration_seconds_bucket{le="0.025"} 1234
gateway_http_request_duration_seconds_bucket{le="0.050"} 1456
gateway_http_request_duration_seconds_bucket{le="+Inf"} 1523
gateway_http_request_duration_seconds_sum 45.678
gateway_http_request_duration_seconds_count 1523

# HELP gateway_http_upstream_duration_seconds Upstream service latency in seconds
# TYPE gateway_http_upstream_duration_seconds histogram
gateway_http_upstream_duration_seconds_bucket{le="0.010"} 789
gateway_http_upstream_duration_seconds_bucket{le="+Inf"} 1523
gateway_http_upstream_duration_seconds_sum 38.901
gateway_http_upstream_duration_seconds_count 1523
```

**Prometheus 配置示例：**

```yaml
scrape_configs:
  - job_name: 'gateway'
    static_configs:
      - targets: ['localhost:9090']
```

---

### 3. 分布式追踪 (Tracing)

```json
{
  "gateway": {
    "observability": {
      "enable_tracing": 1,              // 追踪开关
      "tracing_exporter": "console",    // 导出器类型
      "tracing_endpoint": "http://localhost:14268/api/traces",  // 导出端点
      "tracing_sample_rate": 1.0        // 采样率 (0.0-1.0)
    }
  }
}
```

**参数详解：**
- `enable_tracing`: 关闭或开启分布式追踪
- `tracing_exporter`: 
  - `console`: 输出到控制台（调试用）
  - `jaeger`: Jaeger HTTP API
  - `zipkin`: Zipkin HTTP API
  - `otlp`: OpenTelemetry Protocol
- `tracing_endpoint`: 追踪收集器的 URL
- `tracing_sample_rate`: 
  - `1.0`: 100% 采样（所有请求都追踪）
  - `0.1`: 10% 采样
  - `0.01`: 1% 采样（高流量场景推荐）

**支持的追踪标准：**
- ✅ W3C Trace Context (`traceparent` header)
- ✅ 兼容 Jaeger/Zipkin/OpenTelemetry

**Traceparent 头格式：**
```
traceparent: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01
             │─version─││────── trace ID ──────││─span ID─││flags│
```

---

## 性能优化建议

### 1. 日志性能
- ✅ 生产环境使用 `info` 级别，避免 `debug` 级别的性能开销
- ✅ 使用 JSON 格式时，确保日志收集系统支持（如 Filebeat + Elasticsearch）
- ✅ 高并发场景可降低采样率

### 2. 指标性能
- ✅ Prometheus 指标使用线程安全的锁机制
- ✅ 直方图桶的数量已优化，避免过多内存占用
- ✅ 指标服务器独立运行在工作线程，不影响主业务

### 3. 追踪性能
- ✅ 生产环境建议使用采样（如 10% 或 1%）
- ✅ 异步导出追踪数据，避免阻塞请求
- ✅ 高流量场景推荐使用 OTLP 协议（更高效）

---

## 快速开始

### 开发环境（全量开启）

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 1,
      "log_level": "debug",
      "enable_json_log": 0,
      
      "enable_metrics": 1,
      "metrics_port": 9090,
      "metrics_path": "/metrics",
      
      "enable_tracing": 1,
      "tracing_exporter": "console",
      "tracing_sample_rate": 1.0
    }
  }
}
```

### 生产环境（推荐配置）

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
      "tracing_endpoint": "http://jaeger-collector:14268/api/traces",
      "tracing_sample_rate": 0.1
    }
  }
}
```

### 高性能模式（降低开销）

```json
{
  "gateway": {
    "observability": {
      "enable_logging": 1,
      "log_level": "warn",
      "enable_json_log": 1,
      
      "enable_metrics": 1,
      "metrics_port": 9090,
      "metrics_path": "/metrics",
      
      "enable_tracing": 1,
      "tracing_exporter": "otlp",
      "tracing_endpoint": "http://otel-collector:4317/v1/traces",
      "tracing_sample_rate": 0.01
    }
  }
}
```

---

## Grafana 仪表盘示例

导入以下面板 ID 到 Grafana：

### 请求速率和延迟
```prometheus
# 请求速率 (QPS)
rate(gateway_http_requests_total[1m])

# P50/P95/P99延迟
histogram_quantile(0.50, rate(gateway_http_request_duration_seconds_bucket[1m]))
histogram_quantile(0.95, rate(gateway_http_request_duration_seconds_bucket[1m]))
histogram_quantile(0.99, rate(gateway_http_request_duration_seconds_bucket[1m]))

# 错误率
rate(gateway_http_request_errors_total[1m]) / rate(gateway_http_requests_total[1m])
```

---

## 故障排查

### 1. 指标不显示
- 检查 `enable_metrics` 是否为 1
- 确认 `metrics_port` 未被占用
- 访问 `http://localhost:9090/metrics` 验证

### 2. 日志不输出
- 检查 `enable_logging` 是否为 1
- 确认 `log_level` 设置正确
- 检查日志文件路径权限

### 3. 追踪数据丢失
- 确认 `tracing_endpoint` 地址可达
- 检查采样率是否过低
- 验证 `traceparent` 头是否正确传递

---

## 下一步计划

未来将支持：
- [ ] 认证中间件（JWT/API Key）
- [ ] 限流中间件（令牌桶算法）
- [ ] Redis 分布式限流
- [ ] 更多 Prometheus 指标类型
- [ ] OpenTelemetry 原生集成

---

## 技术细节

如需了解实现细节，请参考源代码：
- `src/logger.c` - 日志模块
- `src/metrics.c` - Prometheus 指标模块
- `src/tracer.c` - 分布式追踪模块
- `include/gateway.h` - 数据结构定义
