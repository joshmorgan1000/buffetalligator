#include <cswift/detail/telemetry.hpp>

namespace cswift {

std::unique_ptr<CSTelemetryEngine> CSTelemetryEngine::create(CSTelemetryLevel level) {
    
    CSHandle handle = cswift_telemetry_engine_create(static_cast<int32_t>(level));
    
    if (!handle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create telemetry engine");
    }
    
    return std::unique_ptr<CSTelemetryEngine>(new CSTelemetryEngine(handle));
}

CSTelemetryEngine::~CSTelemetryEngine() {
    if (handle_) {
        cswift_telemetry_engine_destroy(handle_);
    }
}

// Move-only semantics
CSTelemetryEngine::CSTelemetryEngine(CSTelemetryEngine&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

CSTelemetryEngine& CSTelemetryEngine::operator=(CSTelemetryEngine&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            cswift_telemetry_engine_destroy(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

/**
 * @brief Start real-time telemetry collection
 * 
 * Begins monitoring system resources, performance metrics, and collecting
 * real-time data for analysis and alerting.
 * 
 * @throw CSException on start failure
 */
void CSTelemetryEngine::start() {
    CSError result = static_cast<CSError>(cswift_telemetry_start(handle_));
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to start telemetry collection");
    }
}

/**
 * @brief Stop telemetry collection
 * 
 * Stops all monitoring and data collection. Existing metrics are preserved.
 * 
 * @throw CSException on stop failure
 */
void CSTelemetryEngine::stop() {
    CSError result = static_cast<CSError>(cswift_telemetry_stop(handle_));
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to stop telemetry collection");
    }
}

/**
 * @brief Record a custom performance metric
 * 
 * Records a custom metric value for monitoring and analysis. Metrics are
 * automatically aggregated and can trigger alerts.
 * 
 * @param name Metric name (e.g., "api_requests", "response_time")
 * @param value Metric value
 * @param type Type of metric (default: Gauge)
 * @param resourceType Resource category (default: Custom)
 * @throw CSException on recording failure
 */
void CSTelemetryEngine::recordMetric(const std::string& name, double value, 
                    CSMetricType type,
                    CSResourceType resourceType) {
    
    CSError result = static_cast<CSError>(cswift_telemetry_record_metric(
        handle_, 
        name.c_str(), 
        value,
        static_cast<int32_t>(type),
        static_cast<int32_t>(resourceType)
    ));
    
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to record metric: " + name);
    }
}

/**
 * @brief Time an operation and record the duration
 * 
 * Automatically measures the execution time of an operation and records
 * it as a timing metric. This is ideal for monitoring API calls, database
 * queries, ML inference, etc.
 * 
 * @param name Operation name
 * @param operation Function to time
 * @return Result of the operation
 */
template<typename Func>
auto CSTelemetryEngine::timeOperation(const std::string& name, Func&& operation) -> decltype(operation()) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if constexpr (std::is_void_v<decltype(operation())>) {
        operation();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        recordMetric(name + "_duration_us", static_cast<double>(duration.count()), CSMetricType::Timer);
    } else {
        auto result = operation();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        recordMetric(name + "_duration_us", static_cast<double>(duration.count()), CSMetricType::Timer);
        return result;
    }
}

/**
 * @brief Set alert threshold for a metric
 * 
 * Configures automatic alerting when a metric exceeds a threshold.
 * Alerts can be exported to external monitoring systems.
 * 
 * @param metricName Name of metric to monitor
 * @param threshold Alert threshold value
 * @param severity Alert severity level
 * @throw CSException on configuration failure
 */
void CSTelemetryEngine::setAlert(const std::string& metricName, double threshold, 
                CSAlertSeverity severity) {
    
    CSError result = static_cast<CSError>(cswift_telemetry_set_alert(
        handle_, 
        metricName.c_str(), 
        threshold,
        static_cast<int32_t>(severity)
    ));
    
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to set alert for metric: " + metricName);
    }
}

/**
 * @brief Get overall system health score
 * 
 * Returns a health score (0-100) based on all monitored metrics.
 * This provides a quick overview of system performance and issues.
 * 
 * @return Health score (100 = excellent, 0 = critical issues)
 */
float CSTelemetryEngine::getHealthScore() const {
    return cswift_telemetry_get_health_score(handle_);
}

/**
 * @brief Add console output for metrics
 * 
 * Enables console logging of metrics for development and debugging.
 * 
 * @throw CSException on configuration failure
 */
void CSTelemetryEngine::addConsoleExporter() {
    CSError result = static_cast<CSError>(cswift_telemetry_add_console_exporter(handle_));
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to add console exporter");
    }
}

/**
 * @brief Increment a counter metric
 * 
 * Convenience method for counter metrics (monotonically increasing values).
 */
void CSTelemetryEngine::incrementCounter(const std::string& name, double increment) {
    recordMetric(name, increment, CSMetricType::Counter);
}

/**
 * @brief Record a rate metric
 * 
 * Convenience method for rate metrics (events per unit time).
 */
void CSTelemetryEngine::recordRate(const std::string& name, double rate) {
    recordMetric(name, rate, CSMetricType::Rate);
}

CSHandle CSTelemetryEngine::getHandle() const { return handle_; }

CSTelemetryEngine::CSTelemetryEngine(CSHandle handle) : handle_(handle) {}

} // namespace cswift