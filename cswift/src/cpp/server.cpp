#include <cswift/detail/server.hpp>

namespace cswift {

/**
 * @brief Create WebSocket ML serving server
 * 
 * @param port Port to listen on (default: 8080)
 * @param protocol Serving protocol (default: Realtime)
 * @throw CSException on initialization failure
 */
std::unique_ptr<CSWebSocketMLServer> CSWebSocketMLServer::create(
    uint16_t port, 
    CSMLServingProtocol protocol) {
    
    void* error = nullptr;
    CSHandle handle = cswift_websocket_ml_server_create(
        port, 
        static_cast<int32_t>(protocol), 
        &error
    );
    
    if (!handle) {
        std::string errorMsg = "Failed to create WebSocket ML server";
        if (error) {
            errorMsg = static_cast<const char*>(error);
            free(error);
        }
        throw CSException(CS_ERROR_OPERATION_FAILED, errorMsg);
    }
    
    return std::unique_ptr<CSWebSocketMLServer>(new CSWebSocketMLServer(handle));
}

CSWebSocketMLServer::~CSWebSocketMLServer() {
    if (handle_) {
        cswift_websocket_ml_server_destroy(handle_);
    }
}

// Move-only semantics
CSWebSocketMLServer::CSWebSocketMLServer(CSWebSocketMLServer&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

CSWebSocketMLServer& CSWebSocketMLServer::operator=(CSWebSocketMLServer&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            cswift_websocket_ml_server_destroy(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

/**
 * @brief Start the WebSocket ML serving server
 * 
 * Begins listening for WebSocket connections and serving ML inference
 * requests in real-time. The server will handle concurrent connections
 * and process inference requests with priority-based scheduling.
 * 
 * @throw CSException on start failure
 */
void CSWebSocketMLServer::start() {
    CSError result = static_cast<CSError>(cswift_websocket_ml_server_start(handle_));
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to start WebSocket ML server");
    }
}

/**
 * @brief Stop the WebSocket ML serving server
 * 
 * Gracefully shuts down the server, closes all client connections,
 * and stops processing inference requests.
 * 
 * @throw CSException on stop failure
 */
void CSWebSocketMLServer::stop() {
    CSError result = static_cast<CSError>(cswift_websocket_ml_server_stop(handle_));
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to stop WebSocket ML server");
    }
}

/**
 * @brief Register ML model for real-time serving
 * 
 * Registers a trained ML model to be served via WebSocket connections.
 * Models can be neural networks, decision trees, or any other ML model
 * that supports real-time inference.
 * 
 * @param modelId Unique identifier for the model
 * @param model Handle to the trained ML model
 * @throw CSException on registration failure
 */
void CSWebSocketMLServer::registerModel(const std::string& modelId, CSHandle model) {
    CSError result = static_cast<CSError>(cswift_websocket_ml_server_register_model(
        handle_, 
        modelId.c_str(), 
        model
    ));
    
    if (result != CS_SUCCESS) {
        throw CSException(result, "Failed to register ML model: " + modelId);
    }
}

} // namespace cswift
