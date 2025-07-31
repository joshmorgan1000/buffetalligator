import Foundation

#if canImport(MetalPerformanceShaders) && canImport(MetalPerformanceShadersGraph)
import Metal
import MetalPerformanceShaders
import MetalPerformanceShadersGraph
#else
// Stub implementation for non-Apple platforms
enum MPSDataType: Int32 {
    case float32 = 0
    case float16 = 1
    case int32 = 2
    case int64 = 3
    case bool = 4
}

class MPSGraph {
    init() {}
}

class MPSGraphTensor {
    let name: String?
    let shape: [Int]
    let dataType: MPSDataType
    
    init(name: String?, shape: [Int], dataType: MPSDataType) {
        self.name = name
        self.shape = shape
        self.dataType = dataType
    }
}

class MPSGraphDevice {
    static func metalDevice() -> MPSGraphDevice? {
        return MPSGraphDevice()
    }
}

class MPSGraphExecutable {
    init() {}
}
#endif

/**
 * @brief Wrapper for MPSGraph
 */
class GraphWrapper {
    #if canImport(MetalPerformanceShadersGraph)
    let graph: MPSGraph
    #else
    let graph: MPSGraph
    #endif
    
    private var tensors: [String: TensorWrapper] = [:]
    
    init() {
        #if canImport(MetalPerformanceShadersGraph)
        self.graph = MPSGraph()
        #else
        self.graph = MPSGraph()
        #endif
    }
    
    func addTensor(_ name: String, tensor: TensorWrapper) {
        tensors[name] = tensor
    }
    
    func getTensor(_ name: String) -> TensorWrapper? {
        return tensors[name]
    }
}

/**
 * @brief Wrapper for MPSGraphTensor
 */
class TensorWrapper {
    #if canImport(MetalPerformanceShadersGraph)
    let tensor: MPSGraphTensor
    #else
    let tensor: MPSGraphTensor
    #endif
    
    init(_ tensor: MPSGraphTensor) {
        self.tensor = tensor
    }
}

/**
 * @brief Wrapper for MPSGraphDevice
 */
class DeviceWrapper {
    #if canImport(MetalPerformanceShadersGraph)
    let device: MPSGraphDevice
    #else
    let device: MPSGraphDevice?
    #endif
    
    init?() {
        #if canImport(MetalPerformanceShadersGraph)
        guard let metalDevice = MTLCreateSystemDefaultDevice() else {
            return nil
        }
        self.device = MPSGraphDevice(mtlDevice: metalDevice)
        #else
        self.device = MPSGraphDevice.metalDevice()
        #endif
    }
}

/**
 * @brief Create a new MPSGraph
 * 
 * @return Opaque pointer to GraphWrapper
 */
@_cdecl("cswift_mps_graph_create")
func cswift_mps_graph_create() -> OpaquePointer {
    let wrapper = GraphWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Destroy an MPSGraph
 * 
 * @param handle Graph handle
 */
@_cdecl("cswift_mps_graph_destroy")
func cswift_mps_graph_destroy(_ handle: OpaquePointer) {
    _ = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeRetainedValue()
}

/**
 * @brief Create a placeholder tensor
 * 
 * @param handle Graph handle
 * @param shape Array of dimensions
 * @param shapeCount Number of dimensions
 * @param dataType Data type
 * @param name Optional name
 * @return Opaque pointer to TensorWrapper or NULL on error
 */
@_cdecl("cswift_mps_graph_placeholder")
func cswift_mps_graph_placeholder(
    _ handle: OpaquePointer,
    _ shape: UnsafePointer<Int32>,
    _ shapeCount: Int,
    _ dataType: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let wrapper = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeUnretainedValue()
    
    // Convert shape to NSNumber array
    var shapeArray: [NSNumber] = []
    for i in 0..<shapeCount {
        shapeArray.append(NSNumber(value: shape[i]))
    }
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    // Convert data type
    let mpsDataType: MPSDataType
    switch dataType {
    case 0: mpsDataType = .float32
    case 1: mpsDataType = .float16
    case 2: mpsDataType = .int32
    case 3: mpsDataType = .int64
    case 4: mpsDataType = .bool
    default: mpsDataType = .float32
    }
    
    let tensor = wrapper.graph.placeholder(
        shape: shapeArray,
        dataType: mpsDataType,
        name: nameStr
    )
    #else
    let tensor = MPSGraphTensor(
        name: nameStr,
        shape: shapeArray.map { $0.intValue },
        dataType: MPSDataType(rawValue: dataType) ?? .float32
    )
    #endif
    
    let tensorWrapper = TensorWrapper(tensor)
    if let name = nameStr {
        wrapper.addTensor(name, tensor: tensorWrapper)
    }
    
    return Unmanaged.passRetained(tensorWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Matrix multiplication operation
 * 
 * @param handle Graph handle
 * @param aHandle First tensor
 * @param bHandle Second tensor
 * @param transposeA Whether to transpose A
 * @param transposeB Whether to transpose B
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_matmul")
func cswift_mps_graph_matmul(
    _ handle: OpaquePointer,
    _ aHandle: OpaquePointer,
    _ bHandle: OpaquePointer,
    _ transposeA: Int32,
    _ transposeB: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let wrapper = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeUnretainedValue()
    let aTensor = Unmanaged<TensorWrapper>.fromOpaque(aHandle.toRawPointer()).takeUnretainedValue()
    let bTensor = Unmanaged<TensorWrapper>.fromOpaque(bHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    var primaryTensor = aTensor.tensor
    var secondaryTensor = bTensor.tensor
    
    // Apply transposition if needed
    if transposeA != 0 {
        primaryTensor = wrapper.graph.transposeTensor(
            primaryTensor,
            dimension: primaryTensor.shape!.count - 2,
            withDimension: primaryTensor.shape!.count - 1,
            name: nil
        )
    }
    
    if transposeB != 0 {
        secondaryTensor = wrapper.graph.transposeTensor(
            secondaryTensor,
            dimension: secondaryTensor.shape!.count - 2,
            withDimension: secondaryTensor.shape!.count - 1,
            name: nil
        )
    }
    
    let result = wrapper.graph.matrixMultiplication(
        primary: primaryTensor,
        secondary: secondaryTensor,
        name: nameStr
    )
    #else
    // Stub implementation
    let result = MPSGraphTensor(
        name: nameStr,
        shape: [1, 1],
        dataType: aTensor.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        wrapper.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief ReLU activation function
 * 
 * @param handle Graph handle
 * @param inputHandle Input tensor
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_relu")
func cswift_mps_graph_relu(
    _ handle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let wrapper = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeUnretainedValue()
    let inputTensor = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = wrapper.graph.reLU(
        with: inputTensor.tensor,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: inputTensor.tensor.shape,
        dataType: inputTensor.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        wrapper.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Addition operation
 * 
 * @param handle Graph handle
 * @param aHandle First tensor
 * @param bHandle Second tensor
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_addition")
func cswift_mps_graph_addition(
    _ handle: OpaquePointer,
    _ aHandle: OpaquePointer,
    _ bHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let wrapper = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeUnretainedValue()
    let aTensor = Unmanaged<TensorWrapper>.fromOpaque(aHandle.toRawPointer()).takeUnretainedValue()
    let bTensor = Unmanaged<TensorWrapper>.fromOpaque(bHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = wrapper.graph.addition(
        aTensor.tensor,
        bTensor.tensor,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: aTensor.tensor.shape,
        dataType: aTensor.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        wrapper.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Get tensor by name
 * 
 * @param handle Graph handle
 * @param name Tensor name
 * @return Tensor handle or NULL if not found
 */
@_cdecl("cswift_mps_graph_get_tensor")
func cswift_mps_graph_get_tensor(
    _ handle: OpaquePointer,
    _ name: UnsafePointer<CChar>
) -> OpaquePointer? {
    let wrapper = Unmanaged<GraphWrapper>.fromOpaque(handle.toRawPointer()).takeUnretainedValue()
    let nameStr = String(cString: name)
    
    guard let tensor = wrapper.getTensor(nameStr) else {
        return nil
    }
    
    // Return unretained since the graph owns the tensor
    return Unmanaged.passUnretained(tensor).toOpaque().toOpaquePointer()
}

/**
 * @brief Destroy a tensor handle
 * 
 * @param handle Tensor handle
 */
@_cdecl("cswift_mps_graph_tensor_destroy")
func cswift_mps_graph_tensor_destroy(_ handle: OpaquePointer) {
    _ = Unmanaged<TensorWrapper>.fromOpaque(handle.toRawPointer()).takeRetainedValue()
}

/**
 * @brief Create a default Metal device
 * 
 * @return Device handle or NULL if not available
 */
@_cdecl("cswift_mps_device_create_default")
func cswift_mps_device_create_default() -> OpaquePointer? {
    guard let wrapper = DeviceWrapper() else {
        return nil
    }
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Destroy a device handle
 * 
 * @param handle Device handle
 */
@_cdecl("cswift_mps_device_destroy")
func cswift_mps_device_destroy(_ handle: OpaquePointer) {
    _ = Unmanaged<DeviceWrapper>.fromOpaque(handle.toRawPointer()).takeRetainedValue()
}

// MARK: - MPSGraphExecutable Implementation

/**
 * @brief Wrapper for MPSGraphExecutable
 */
class MPSGraphExecutableWrapper {
    #if canImport(MetalPerformanceShadersGraph)
    let executable: MPSGraphExecutable?
    let feeds: [MPSGraphTensor]
    let targetTensors: [MPSGraphTensor]
    let targetOperations: [MPSGraphOperation]
    #else
    var feeds: [String] = []
    var targets: [String] = []
    #endif
    
    #if canImport(MetalPerformanceShadersGraph)
    init(executable: MPSGraphExecutable, feeds: [MPSGraphTensor], targets: [MPSGraphTensor], operations: [MPSGraphOperation] = []) {
        self.executable = executable
        self.feeds = feeds
        self.targetTensors = targets
        self.targetOperations = operations
    }
    #endif
    
    init() {
        #if canImport(MetalPerformanceShadersGraph)
        self.executable = nil
        self.feeds = []
        self.targetTensors = []
        self.targetOperations = []
        #endif
    }
}

/**
 * @brief Compile MPS graph into executable
 * 
 * @param graphHandle Graph handle
 * @param deviceHandle Device handle
 * @param feedTensorHandles Array of feed tensor handles
 * @param feedCount Number of feed tensors
 * @param targetTensorHandles Array of target tensor handles
 * @param targetCount Number of target tensors
 * @param error Error info output
 * @return Executable handle or NULL on error
 */
@_cdecl("cswift_mps_graph_compile")
func cswift_mps_graph_compile(
    _ graphHandle: OpaquePointer,
    _ deviceHandle: OpaquePointer,
    _ feedTensorHandles: UnsafePointer<OpaquePointer>,
    _ feedCount: Int32,
    _ targetTensorHandles: UnsafePointer<OpaquePointer>,
    _ targetCount: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let graphWrapper = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let deviceWrapper = Unmanaged<DeviceWrapper>.fromOpaque(deviceHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(MetalPerformanceShadersGraph)
    let device = deviceWrapper.device
    
    // Convert handles to tensors and create feed dictionary
    var feedTensors: [MPSGraphTensor] = []
    var targetTensors: [MPSGraphTensor] = []
    var feedDict: [MPSGraphTensor: MPSGraphShapedType] = [:]
    
    for i in 0..<Int(feedCount) {
        let tensorWrapper = Unmanaged<TensorWrapper>.fromOpaque(feedTensorHandles[i].toRawPointer()).takeUnretainedValue()
        feedTensors.append(tensorWrapper.tensor)
        // Create a placeholder shaped type for compilation (actual data provided at execution)
        feedDict[tensorWrapper.tensor] = MPSGraphShapedType(shape: [1], dataType: .float32)
    }
    
    for i in 0..<Int(targetCount) {
        let tensorWrapper = Unmanaged<TensorWrapper>.fromOpaque(targetTensorHandles[i].toRawPointer()).takeUnretainedValue()
        targetTensors.append(tensorWrapper.tensor)
    }
    
    let executable = graphWrapper.graph.compile(with: device, 
                                                   feeds: feedDict, 
                                                   targetTensors: targetTensors, 
                                                   targetOperations: nil, 
                                                   compilationDescriptor: nil)
    
    let executableWrapper = MPSGraphExecutableWrapper(executable: executable, 
                                                     feeds: feedTensors, 
                                                     targets: targetTensors)
    return Unmanaged.passRetained(executableWrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation
    let executableWrapper = MPSGraphExecutableWrapper()
    return Unmanaged.passRetained(executableWrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Execute compiled graph
 * 
 * @param executableHandle Executable handle
 * @param commandBufferHandle Metal command buffer handle (optional)
 * @param inputDataHandles Array of input data handles
 * @param inputCount Number of inputs
 * @param outputDataHandles Array for output data handles (caller allocated)
 * @param outputCount Number of expected outputs
 * @param error Error info output
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_mps_graph_executable_run")
func cswift_mps_graph_executable_run(
    _ executableHandle: OpaquePointer,
    _ commandBufferHandle: OpaquePointer?,
    _ inputDataHandles: UnsafePointer<OpaquePointer>,
    _ inputCount: Int32,
    _ outputDataHandles: UnsafeMutablePointer<OpaquePointer?>,
    _ outputCount: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> Int32 {
    let executableWrapper = Unmanaged<MPSGraphExecutableWrapper>.fromOpaque(executableHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(MetalPerformanceShadersGraph)
    guard let executable = executableWrapper.executable else {
        return CS_ERROR_INVALID_ARGUMENT
    }
    
    // Convert input handles to MLMTensor data
    var inputsArray: [MPSGraphTensorData] = []
    
    for i in 0..<Int(inputCount) {
        let bufferWrapper = Unmanaged<MTLBufferWrapper>.fromOpaque(inputDataHandles[i].toRawPointer()).takeUnretainedValue()
        
        if let buffer = bufferWrapper.buffer {
            // Create MPSGraphTensorData from MTL buffer
            let tensorData = MPSGraphTensorData(buffer, 
                                              shape: [1, 1, 1, 1], // Shape should be provided separately
                                              dataType: .float32)
            inputsArray.append(tensorData)
        }
    }
    
    let commandQueue = commandBufferHandle != nil ? 
        Unmanaged<MTLCommandBufferWrapper>.fromOpaque(commandBufferHandle!.toRawPointer()).takeUnretainedValue().commandBuffer?.commandQueue :
        nil
    
    let outputs = executable.run(with: commandQueue!, 
                                   inputs: inputsArray, 
                                   results: nil, 
                                   executionDescriptor: nil)
    
    // Store output handles
    let outputCount = min(outputs.count, Int(outputCount))
    for i in 0..<outputCount {
        let outputData = outputs[i]
        // For now, just return the MPSGraphTensorData as opaque pointer
        // In a real implementation, you'd convert back to MTL buffer
        outputDataHandles[i] = Unmanaged.passRetained(outputData).toOpaque().toOpaquePointer()
    }
    
    return CS_SUCCESS
    #else
    // Stub implementation
    for i in 0..<Int(outputCount) {
        outputDataHandles[i] = nil
    }
    return CS_SUCCESS
    #endif
}

/**
 * @brief Execute compiled graph asynchronously
 * 
 * @param executableHandle Executable handle
 * @param commandBufferHandle Metal command buffer handle
 * @param inputDataHandles Array of input data handles
 * @param inputCount Number of inputs
 * @param completionHandler Completion callback (C function pointer)
 * @param context Context for completion callback
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_mps_graph_executable_run_async")
func cswift_mps_graph_executable_run_async(
    _ executableHandle: OpaquePointer,
    _ commandBufferHandle: OpaquePointer,
    _ inputDataHandles: UnsafePointer<OpaquePointer>,
    _ inputCount: Int32,
    _ completionHandler: @escaping @convention(c) (Int32, UnsafeRawPointer?) -> Void,
    _ context: UnsafeRawPointer?
) -> Int32 {
    let executableWrapper = Unmanaged<MPSGraphExecutableWrapper>.fromOpaque(executableHandle.toRawPointer()).takeUnretainedValue()
    let commandBufferWrapper = Unmanaged<MTLCommandBufferWrapper>.fromOpaque(commandBufferHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(MetalPerformanceShadersGraph)
    guard let executable = executableWrapper.executable,
          let commandBuffer = commandBufferWrapper.commandBuffer else {
        completionHandler(CS_ERROR_INVALID_ARGUMENT, context)
        return CS_ERROR_INVALID_ARGUMENT
    }
    
    // Convert input handles to MLMTensor data
    var inputsArray: [MPSGraphTensorData] = []
    
    for i in 0..<Int(inputCount) {
        let bufferWrapper = Unmanaged<MTLBufferWrapper>.fromOpaque(inputDataHandles[i].toRawPointer()).takeUnretainedValue()
        
        if let buffer = bufferWrapper.buffer {
            let tensorData = MPSGraphTensorData(buffer, 
                                              shape: [1, 1, 1, 1], 
                                              dataType: .float32)
            inputsArray.append(tensorData)
        }
    }
    
    // Run asynchronously
    DispatchQueue.global(qos: .userInitiated).async {
        let commandQueue = commandBuffer.commandQueue
        let _ = executable.run(with: commandQueue, 
                                 inputs: inputsArray, 
                                 results: nil, 
                                 executionDescriptor: nil)
        
        // Wait for command buffer completion
        commandBuffer.waitUntilCompleted()
        
        completionHandler(CS_SUCCESS, context)
    }
    
    return CS_SUCCESS
    #else
    // Stub implementation
    DispatchQueue.global().async {
        completionHandler(CS_SUCCESS, context)
    }
    return CS_SUCCESS
    #endif
}

/**
 * @brief Get executable optimization options
 * 
 * @param executableHandle Executable handle
 * @param outOptimizationProfile Optimization profile output
 * @param outCompileTime Compilation time output (milliseconds)
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_mps_graph_executable_options")
func cswift_mps_graph_executable_options(
    _ executableHandle: OpaquePointer,
    _ outOptimizationProfile: UnsafeMutablePointer<Int32>,
    _ outCompileTime: UnsafeMutablePointer<Double>
) -> Int32 {
    let executableWrapper = Unmanaged<MPSGraphExecutableWrapper>.fromOpaque(executableHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(MetalPerformanceShadersGraph)
    guard let executable = executableWrapper.executable else {
        return CS_ERROR_INVALID_ARGUMENT
    }
    
    // Get compilation options (iOS 15.0+, macOS 12.0+)
    if #available(iOS 15.0, macOS 12.0, *) {
        let options = executable.options
        outOptimizationProfile.pointee = Int32(options.rawValue)
    } else {
        outOptimizationProfile.pointee = 0
    }
    
    // Compilation time would need to be tracked during compilation
    outCompileTime.pointee = 0.0
    
    return CS_SUCCESS
    #else
    // Stub implementation
    outOptimizationProfile.pointee = 0
    outCompileTime.pointee = 0.0
    return CS_SUCCESS
    #endif
}

/**
 * @brief Destroy executable
 * 
 * @param executableHandle Executable handle
 */
@_cdecl("cswift_mps_graph_executable_destroy")
func cswift_mps_graph_executable_destroy(_ executableHandle: OpaquePointer) {
    _ = Unmanaged<MPSGraphExecutableWrapper>.fromOpaque(executableHandle.toRawPointer()).takeRetainedValue()
}

// MARK: - MTLCommandBuffer Implementation (needed for executable)

/**
 * @brief Wrapper for MTLCommandBuffer
 */
class MTLCommandBufferWrapper {
    #if canImport(Metal)
    let commandBuffer: MTLCommandBuffer?
    #endif
    
    #if canImport(Metal)
    init(commandBuffer: MTLCommandBuffer) {
        self.commandBuffer = commandBuffer
    }
    #endif
    
    init() {
        #if canImport(Metal)
        self.commandBuffer = nil
        #endif
    }
}

/**
 * @brief Create command buffer from command queue
 * 
 * @param queueHandle Command queue handle
 * @return Command buffer handle or NULL on error
 */
@_cdecl("cswift_mtl_command_buffer_create")
func cswift_mtl_command_buffer_create(_ queueHandle: OpaquePointer) -> OpaquePointer? {
    let queueWrapper = Unmanaged<MTLCommandQueueWrapper>.fromOpaque(queueHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    let queue = queueWrapper.queue
    
    guard let commandBuffer = queue.makeCommandBuffer() else {
        return nil
    }
    
    let wrapper = MTLCommandBufferWrapper(commandBuffer: commandBuffer)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation
    let wrapper = MTLCommandBufferWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Commit command buffer for execution
 * 
 * @param commandBufferHandle Command buffer handle
 */
@_cdecl("cswift_mtl_command_buffer_commit")
func cswift_mtl_command_buffer_commit(_ commandBufferHandle: OpaquePointer) {
    let wrapper = Unmanaged<MTLCommandBufferWrapper>.fromOpaque(commandBufferHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    wrapper.commandBuffer?.commit()
    #endif
}

/**
 * @brief Wait for command buffer completion
 * 
 * @param commandBufferHandle Command buffer handle
 */
@_cdecl("cswift_mtl_command_buffer_wait_until_completed")
func cswift_mtl_command_buffer_wait_until_completed(_ commandBufferHandle: OpaquePointer) {
    let wrapper = Unmanaged<MTLCommandBufferWrapper>.fromOpaque(commandBufferHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    wrapper.commandBuffer?.waitUntilCompleted()
    #endif
}

/**
 * @brief Destroy command buffer
 * 
 * @param commandBufferHandle Command buffer handle
 */
@_cdecl("cswift_mtl_command_buffer_destroy")
func cswift_mtl_command_buffer_destroy(_ commandBufferHandle: OpaquePointer) {
    _ = Unmanaged<MTLCommandBufferWrapper>.fromOpaque(commandBufferHandle.toRawPointer()).takeRetainedValue()
}