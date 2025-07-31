#ifndef CSWIFT_PARAMETERS_HPP
#define CSWIFT_PARAMETERS_HPP

#include <cswift/detail/globals.hpp>
#include <cswift/detail/network.hpp>

namespace cswift {

/**
 * @brief Network parameters for connections and listeners
 */
class CSNWParameters {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Create TCP parameters
     */
    static std::unique_ptr<CSNWParameters> tcp() {
        CSHandle handle = cswift_nw_parameters_create_tcp();
        return std::unique_ptr<CSNWParameters>(new CSNWParameters(handle));
    }
    
    /**
     * @brief Create UDP parameters
     */
    static std::unique_ptr<CSNWParameters> udp() {
        CSHandle handle = cswift_nw_parameters_create_udp();
        return std::unique_ptr<CSNWParameters>(new CSNWParameters(handle));
    }
    
    /**
     * @brief Create secure TCP parameters with TLS
     */
    static std::unique_ptr<CSNWParameters> secureTCP() {
        CSHandle handle = cswift_nw_parameters_create_secure_tcp();
        return std::unique_ptr<CSNWParameters>(new CSNWParameters(handle));
    }
    
    /**
     * @brief Create WebSocket parameters
     */
    static std::unique_ptr<CSNWParameters> webSocket() {
        CSHandle handle = cswift_nw_parameters_create_websocket();
        return std::unique_ptr<CSNWParameters>(new CSNWParameters(handle));
    }
    
    /**
     * @brief Create high-performance parameters optimized for Thunderbolt/10GbE
     * 
     * @details Optimizes for high-bandwidth, low-latency scenarios with:
     * - Thunderbolt interface preference
     * - TCP NoDelay enabled (disables Nagle's algorithm)  
     * - Connection reuse and fast open
     * - Optimized keepalive settings
     */
    static std::unique_ptr<CSNWParameters> highPerformance() {
        CSHandle handle = cswift_nw_parameters_create_high_performance();
        return std::unique_ptr<CSNWParameters>(new CSNWParameters(handle));
    }
    
    ~CSNWParameters() {
        if (handle) {
            cswift_nw_parameters_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSNWParameters(CSNWParameters&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSNWParameters& operator=(CSNWParameters&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_nw_parameters_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSNWParameters(const CSNWParameters&) = delete;
    CSNWParameters& operator=(const CSNWParameters&) = delete;
    
    /**
     * @brief Set required interface type
     * 
     * @param interfaceType Interface type to require
     */
    void setRequiredInterfaceType(CSNWInterfaceType interfaceType) {
        cswift_nw_parameters_set_required_interface_type(handle, static_cast<int32_t>(interfaceType));
    }
    
    /**
     * @brief Add custom protocol framer
     * 
     * @param framer Custom framer to add
     */
    void addCustomFramer(const CSNWProtocolFramer& framer);
    
    /**
     * @brief Detect available high-speed interfaces
     * 
     * @return Vector of available interface types suitable for high-performance networking
     */
    static std::vector<CSNWInterfaceType> detectHighSpeedInterfaces() {
        constexpr int32_t maxInterfaces = 10;
        int32_t interfaceTypes[maxInterfaces];
        int32_t actualCount = 0;
        
        int32_t result = cswift_nw_detect_high_speed_interfaces(interfaceTypes, maxInterfaces, &actualCount);
        if (result != CS_SUCCESS) {
            return {};
        }
        
        std::vector<CSNWInterfaceType> interfaces;
        interfaces.reserve(actualCount);
        for (int32_t i = 0; i < actualCount; ++i) {
            interfaces.push_back(static_cast<CSNWInterfaceType>(interfaceTypes[i]));
        }
        return interfaces;
    }
    
    CSHandle getHandle() const { return handle; }
    
private:
    explicit CSNWParameters(CSHandle h) : handle(h) {}
};

/**
 * @brief Dispatch queue for asynchronous operations
 */
class CSDispatchQueue {
private:
    CSHandle handle;
    bool ownsHandle;
    
public:
    /**
     * @brief Create custom dispatch queue
     * 
     * @param label Queue label
     * @param concurrent Whether queue is concurrent
     */
    CSDispatchQueue(const std::string& label, bool concurrent = false) {
        handle = cswift_dispatch_queue_create(label.c_str(), concurrent ? 1 : 0);
        ownsHandle = true;
    }
    
    /**
     * @brief Get main dispatch queue
     */
    static std::shared_ptr<CSDispatchQueue> main() {
        CSHandle mainHandle = cswift_dispatch_queue_main();
        return std::shared_ptr<CSDispatchQueue>(new CSDispatchQueue(mainHandle, false));
    }
    
    ~CSDispatchQueue() {
        if (handle && ownsHandle) {
            cswift_dispatch_queue_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSDispatchQueue(CSDispatchQueue&& other) noexcept 
        : handle(other.handle), ownsHandle(other.ownsHandle) {
        other.handle = nullptr;
        other.ownsHandle = false;
    }
    
    CSDispatchQueue& operator=(CSDispatchQueue&& other) noexcept {
        if (this != &other) {
            if (handle && ownsHandle) {
                cswift_dispatch_queue_destroy(handle);
            }
            handle = other.handle;
            ownsHandle = other.ownsHandle;
            other.handle = nullptr;
            other.ownsHandle = false;
        }
        return *this;
    }
    
    CSDispatchQueue(const CSDispatchQueue&) = delete;
    CSDispatchQueue& operator=(const CSDispatchQueue&) = delete;
    
    CSHandle getHandle() const { return handle; }
    
private:
    explicit CSDispatchQueue(CSHandle h, bool owns) : handle(h), ownsHandle(owns) {}
};

/**
 * @brief Network connection for client connections
 */
class CSNWConnection {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Create connection to host and port
     * 
     * @param host Hostname or IP address
     * @param port Port number
     * @param parameters Connection parameters
     */
    CSNWConnection(const std::string& host, uint16_t port, 
                   const CSNWParameters& parameters) {
        void* error = nullptr;
        handle = cswift_nw_connection_create(host.c_str(), port, parameters.getHandle(), &error);
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create connection to " + host);
        }
    }
    
    /**
     * @brief Create connection with default TCP parameters
     * 
     * @param host Hostname or IP address
     * @param port Port number
     */
    CSNWConnection(const std::string& host, uint16_t port) {
        void* error = nullptr;
        handle = cswift_nw_connection_create(host.c_str(), port, nullptr, &error);
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create connection to " + host);
        }
    }
    
    ~CSNWConnection() {
        if (handle) {
            cswift_nw_connection_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSNWConnection(CSNWConnection&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSNWConnection& operator=(CSNWConnection&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_nw_connection_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSNWConnection(const CSNWConnection&) = delete;
    CSNWConnection& operator=(const CSNWConnection&) = delete;
    
    /**
     * @brief Set state update handler
     * 
     * @param handler Callback for state changes
     */
    template<typename Func>
    void setStateUpdateHandler(Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_connection_set_state_update_handler(handle, 
            [](int32_t state, void* context) {
                auto* h = static_cast<Func*>(context);
                (*h)(static_cast<CSNWConnectionState>(state));
            }, heapHandler);
    }
    
    /**
     * @brief Start connection
     * 
     * @param queue Dispatch queue for callbacks
     */
    void start(const CSDispatchQueue& queue) {
        int32_t result = cswift_nw_connection_start(handle, queue.getHandle());
        if (result != CS_SUCCESS) {
            throw CSException(static_cast<CSError>(result), "Failed to start connection");
        }
    }
    
    /**
     * @brief Start connection on main queue
     */
    void start() {
        int32_t result = cswift_nw_connection_start(handle, nullptr);
        if (result != CS_SUCCESS) {
            throw CSException(static_cast<CSError>(result), "Failed to start connection");
        }
    }
    
    /**
     * @brief Send data asynchronously
     * 
     * @param data Data to send
     * @param handler Completion handler
     */
    template<typename Func>
    void send(const std::vector<uint8_t>& data, Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_connection_send(handle, data.data(), data.size(),
            [](int32_t result, void* context) {
                auto* h = static_cast<Func*>(context);
                (*h)(result == CS_SUCCESS);
                delete h;
            }, heapHandler);
    }
    
    /**
     * @brief Send data asynchronously
     * 
     * @param data Data to send
     * @param size Data size
     * @param handler Completion handler
     */
    template<typename Func>
    void send(const void* data, size_t size, Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_connection_send(handle, data, size,
            [](int32_t result, void* context) {
                auto* h = static_cast<Func*>(context);
                (*h)(result == CS_SUCCESS);
                delete h;
            }, heapHandler);
    }
    
    /**
     * @brief Receive data asynchronously
     * 
     * @param minLength Minimum bytes to receive
     * @param maxLength Maximum bytes to receive
     * @param handler Receive handler
     */
    template<typename Func>
    void receive(size_t minLength, size_t maxLength, Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_connection_receive(handle, minLength, maxLength,
            [](const void* data, size_t size, bool isComplete, int32_t result, void* context) {
                auto* h = static_cast<Func*>(context);
                if (data && size > 0) {
                    std::vector<uint8_t> receivedData(static_cast<const uint8_t*>(data), 
                                                     static_cast<const uint8_t*>(data) + size);
                    (*h)(std::move(receivedData), isComplete, result == CS_SUCCESS);
                } else {
                    (*h)(std::vector<uint8_t>{}, isComplete, result == CS_SUCCESS);
                }
                
                if (isComplete) {
                    delete h;
                }
            }, heapHandler);
    }
    
    /**
     * @brief Cancel connection
     */
    void cancel() {
        cswift_nw_connection_cancel(handle);
    }
    
    CSHandle getHandle() const { return handle; }
    
private:
    friend class CSNWListener;
    
    // Private constructor for internal use
    explicit CSNWConnection(CSHandle h) : handle(h) {}
};

/**
 * @brief Network listener for server functionality
 */
class CSNWListener {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Create listener on port with parameters
     * 
     * @param port Port to listen on
     * @param parameters Listener parameters
     */
    CSNWListener(uint16_t port, const CSNWParameters& parameters) {
        void* error = nullptr;
        handle = cswift_nw_listener_create(port, parameters.getHandle(), &error);
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create listener on port " + std::to_string(port));
        }
    }
    
    /**
     * @brief Create TCP listener on port
     * 
     * @param port Port to listen on
     */
    explicit CSNWListener(uint16_t port) {
        void* error = nullptr;
        handle = cswift_nw_listener_create(port, nullptr, &error);
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create listener on port " + std::to_string(port));
        }
    }
    
    ~CSNWListener() {
        if (handle) {
            cswift_nw_listener_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSNWListener(CSNWListener&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSNWListener& operator=(CSNWListener&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_nw_listener_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSNWListener(const CSNWListener&) = delete;
    CSNWListener& operator=(const CSNWListener&) = delete;
    
    /**
     * @brief Set new connection handler
     * 
     * @param handler Callback for new connections
     */
    template<typename Func>
    void setNewConnectionHandler(Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_listener_set_new_connection_handler(handle,
            [](CSHandle connectionHandle, void* context) {
                auto* h = static_cast<Func*>(context);
                auto connection = std::unique_ptr<CSNWConnection>(new CSNWConnection(connectionHandle));
                (*h)(std::move(connection));
            }, heapHandler);
    }
    
    /**
     * @brief Set state update handler
     * 
     * @param handler Callback for state changes
     */
    template<typename Func>
    void setStateUpdateHandler(Func&& handler) {
        auto* heapHandler = new Func(std::forward<Func>(handler));
        
        cswift_nw_listener_set_state_update_handler(handle,
            [](int32_t state, void* context) {
                auto* h = static_cast<Func*>(context);
                (*h)(static_cast<CSNWConnectionState>(state));
            }, heapHandler);
    }
    
    /**
     * @brief Start listener
     * 
     * @param queue Dispatch queue for callbacks
     */
    void start(const CSDispatchQueue& queue) {
        int32_t result = cswift_nw_listener_start(handle, queue.getHandle());
        if (result != CS_SUCCESS) {
            throw CSException(static_cast<CSError>(result), "Failed to start listener");
        }
    }
    
    /**
     * @brief Start listener on main queue
     */
    void start() {
        int32_t result = cswift_nw_listener_start(handle, nullptr);
        if (result != CS_SUCCESS) {
            throw CSException(static_cast<CSError>(result), "Failed to start listener");
        }
    }
    
    /**
     * @brief Stop listener
     */
    void stop() {
        cswift_nw_listener_stop(handle);
    }
    
    CSHandle getHandle() const { return handle; }
};

/**
 * @brief Custom protocol framer for zero-copy message protocols
 */
class CSNWProtocolFramer {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Create custom protocol framer
     * 
     * @param identifier Protocol identifier string
     */
    explicit CSNWProtocolFramer(const std::string& identifier) {
        handle = cswift_nw_protocol_framer_create(identifier.c_str());
        if (!handle) {
            throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create protocol framer");
        }
    }
    
    ~CSNWProtocolFramer() {
        if (handle) {
            cswift_nw_protocol_framer_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSNWProtocolFramer(CSNWProtocolFramer&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSNWProtocolFramer& operator=(CSNWProtocolFramer&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_nw_protocol_framer_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSNWProtocolFramer(const CSNWProtocolFramer&) = delete;
    CSNWProtocolFramer& operator=(const CSNWProtocolFramer&) = delete;
    
    CSHandle getHandle() const { return handle; }
};

} // namespace cswift

#endif // CSWIFT_PARAMETERS_HPP
