#ifndef CSWIFT_TELEMETRY_HPP
#define CSWIFT_TELEMETRY_HPP

#include <cswift/detail/globals.hpp>

namespace cswift {

// ============================================================================
// Real-Time Performance Telemetry and Profiling
// ============================================================================

/**
 * @brief Telemetry collection levels
 */
enum class CSTelemetryLevel {
    Off = 0,           ///< No telemetry collection
    Basic = 1,         ///< Basic metrics only
    Detailed = 2,      ///< Detailed performance data
    Diagnostic = 3,    ///< Full diagnostic information
    Debug = 4          ///< Maximum verbosity for debugging
};

/**
 * @brief Performance metric types
 */
enum class CSMetricType {
    Counter = 0,       ///< Monotonic counter
    Gauge = 1,         ///< Point-in-time value
    Histogram = 2,     ///< Distribution of values
    Timer = 3,         ///< Duration measurements
    Rate = 4           ///< Rate over time
};

/**
 * @brief System resource types for monitoring
 */
enum class CSResourceType {
    CPU = 0,           ///< CPU utilization
    Memory = 1,        ///< Memory usage
    GPU = 2,           ///< GPU utilization
    NeuralEngine = 3,  ///< Neural Engine usage
    Network = 4,       ///< Network I/O
    Disk = 5,          ///< Disk I/O
    Custom = 6         ///< Custom application metrics
};

/**
 * @brief Alert severity levels
 */
enum class CSAlertSeverity {
    Info = 0,          ///< Informational
    Warning = 1,       ///< Warning condition
    Error = 2,         ///< Error condition
    Critical = 3       ///< Critical system issue
};

// Performance Telemetry C bridge functions
extern "C" {
    // Telemetry engine functions
    CSHandle cswift_telemetry_engine_create(int32_t level);
    int32_t cswift_telemetry_start(CSHandle engine);
    int32_t cswift_telemetry_stop(CSHandle engine);
    void cswift_telemetry_engine_destroy(CSHandle engine);
    
    // Metric recording
    int32_t cswift_telemetry_record_metric(CSHandle engine, const char* name, double value, 
                                          int32_t type, int32_t resourceType);
    
    // Alerting
    int32_t cswift_telemetry_set_alert(CSHandle engine, const char* metricName, 
                                      double threshold, int32_t severity);
    
    // Health monitoring
    float cswift_telemetry_get_health_score(CSHandle engine);
    
    // Exporters
    int32_t cswift_telemetry_add_console_exporter(CSHandle engine);
}

/**
 * @brief Advanced real-time performance telemetry and profiling system
 * 
 * Provides production-grade performance monitoring with the same observability
 * capabilities used by Netflix, Uber, Google, and Meta for system monitoring.
 * 
 * Features:
 * - Real-time performance metrics collection and analysis
 * - CPU, GPU, and Neural Engine utilization monitoring
 * - Memory usage tracking with leak detection
 * - Network I/O performance monitoring
 * - ML inference latency and throughput tracking
 * - Custom metrics and alerting system
 * - APM (Application Performance Monitoring) integration
 * 
 * Example usage:
 * ```cpp
 * // Create telemetry system with detailed monitoring
 * auto telemetry = CSTelemetryEngine::create(CSTelemetryLevel::Detailed);
 * 
 * // Start real-time monitoring
 * telemetry->start();
 * 
 * // Record custom metrics
 * telemetry->recordMetric("api_requests", 150, CSMetricType::Counter);
 * telemetry->recordMetric("response_time", 45.2, CSMetricType::Timer);
 * 
 * // Set up alerts
 * telemetry->setAlert("cpu_usage", 80.0, CSAlertSeverity::Warning);
 * telemetry->setAlert("memory_usage", 90.0, CSAlertSeverity::Critical);
 * 
 * // Time operations automatically
 * auto result = telemetry->timeOperation("database_query", []() {
 *     return performDatabaseQuery();
 * });
 * 
 * // Get system health
 * float healthScore = telemetry->getHealthScore(); // 0-100
 * ```
 */
class CSTelemetryEngine {
private:
    CSHandle handle_;
    
public:
    /**
     * @brief Create telemetry engine
     * 
     * @param level Telemetry collection level (default: Detailed)
     * @throw CSException on initialization failure
     */
    static std::unique_ptr<CSTelemetryEngine> create(
        CSTelemetryLevel level = CSTelemetryLevel::Detailed);
    
    ~CSTelemetryEngine();
    
    // Move-only semantics
    CSTelemetryEngine(CSTelemetryEngine&& other) noexcept;
    
    CSTelemetryEngine& operator=(CSTelemetryEngine&& other) noexcept;
    
    CSTelemetryEngine(const CSTelemetryEngine&) = delete;
    CSTelemetryEngine& operator=(const CSTelemetryEngine&) = delete;
    
    /**
     * @brief Start real-time telemetry collection
     * 
     * Begins monitoring system resources, performance metrics, and collecting
     * real-time data for analysis and alerting.
     * 
     * @throw CSException on start failure
     */
    void start();
    
    /**
     * @brief Stop telemetry collection
     * 
     * Stops all monitoring and data collection. Existing metrics are preserved.
     * 
     * @throw CSException on stop failure
     */
    void stop();
    
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
    void recordMetric(const std::string& name, double value, 
                     CSMetricType type = CSMetricType::Gauge,
                     CSResourceType resourceType = CSResourceType::Custom);
    
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
    auto timeOperation(const std::string& name, Func&& operation) -> decltype(operation());
    
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
    void setAlert(const std::string& metricName, double threshold, 
                 CSAlertSeverity severity = CSAlertSeverity::Warning);
    
    /**
     * @brief Get overall system health score
     * 
     * Returns a health score (0-100) based on all monitored metrics.
     * This provides a quick overview of system performance and issues.
     * 
     * @return Health score (100 = excellent, 0 = critical issues)
     */
    float getHealthScore() const;
    
    /**
     * @brief Add console output for metrics
     * 
     * Enables console logging of metrics for development and debugging.
     * 
     * @throw CSException on configuration failure
     */
    void addConsoleExporter();
    
    /**
     * @brief Increment a counter metric
     * 
     * Convenience method for counter metrics (monotonically increasing values).
     */
    void incrementCounter(const std::string& name, double increment = 1.0);
    
    /**
     * @brief Record a rate metric
     * 
     * Convenience method for rate metrics (events per unit time).
     */
    void recordRate(const std::string& name, double rate);
    
    CSHandle getHandle() const;

private:
    explicit CSTelemetryEngine(CSHandle handle);
};

} // namespace cswift

#endif // CSWIFT_TELEMETRY_HPP
