import Foundation

#if canImport(MetalPerformanceShadersGraph)
import MetalPerformanceShadersGraph
#endif

/**
 * @brief Convolution 2D operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor handle
 * @param weightsHandle Weights tensor handle
 * @param strideX Stride in X dimension
 * @param strideY Stride in Y dimension
 * @param paddingLeft Left padding
 * @param paddingRight Right padding
 * @param paddingTop Top padding
 * @param paddingBottom Bottom padding
 * @param dilationX Dilation in X dimension
 * @param dilationY Dilation in Y dimension
 * @param groups Number of groups
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_convolution2d")
func cswift_mps_graph_convolution2d(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ weightsHandle: OpaquePointer,
    _ strideX: Int32,
    _ strideY: Int32,
    _ paddingLeft: Int32,
    _ paddingRight: Int32,
    _ paddingTop: Int32,
    _ paddingBottom: Int32,
    _ dilationX: Int32,
    _ dilationY: Int32,
    _ groups: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    let weights = Unmanaged<TensorWrapper>.fromOpaque(weightsHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let descriptor = MPSGraphConvolution2DOpDescriptor(
        strideInX: Int(strideX),
        strideInY: Int(strideY),
        dilationRateInX: Int(dilationX),
        dilationRateInY: Int(dilationY),
        groups: Int(groups),
        paddingLeft: Int(paddingLeft),
        paddingRight: Int(paddingRight),
        paddingTop: Int(paddingTop),
        paddingBottom: Int(paddingBottom),
        paddingStyle: .explicit,
        dataLayout: .NHWC,
        weightsLayout: .HWIO
    )
    
    let result = graph.graph.convolution2D(
        input.tensor,
        weights: weights.tensor,
        descriptor: descriptor!,
        name: nameStr
    )
    #else
    // Stub implementation
    let result = MPSGraphTensor(
        name: nameStr,
        shape: [1, 1, 1, 1],
        dataType: .float32
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Max pooling 2D operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor handle
 * @param kernelWidth Kernel width
 * @param kernelHeight Kernel height
 * @param strideX Stride in X dimension
 * @param strideY Stride in Y dimension
 * @param paddingLeft Left padding
 * @param paddingRight Right padding
 * @param paddingTop Top padding
 * @param paddingBottom Bottom padding
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_maxpool2d")
func cswift_mps_graph_maxpool2d(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ kernelWidth: Int32,
    _ kernelHeight: Int32,
    _ strideX: Int32,
    _ strideY: Int32,
    _ paddingLeft: Int32,
    _ paddingRight: Int32,
    _ paddingTop: Int32,
    _ paddingBottom: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let descriptor = MPSGraphPooling2DOpDescriptor(
        kernelWidth: Int(kernelWidth),
        kernelHeight: Int(kernelHeight),
        strideInX: Int(strideX),
        strideInY: Int(strideY),
        dilationRateInX: 1,
        dilationRateInY: 1,
        paddingLeft: Int(paddingLeft),
        paddingRight: Int(paddingRight),
        paddingTop: Int(paddingTop),
        paddingBottom: Int(paddingBottom),
        paddingStyle: .explicit,
        dataLayout: .NHWC
    )
    
    let result = graph.graph.maxPooling2D(
        withSourceTensor: input.tensor,
        descriptor: descriptor!,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: [1, 1, 1, 1],
        dataType: .float32
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Average pooling 2D operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor handle
 * @param kernelWidth Kernel width
 * @param kernelHeight Kernel height
 * @param strideX Stride in X dimension
 * @param strideY Stride in Y dimension
 * @param paddingLeft Left padding
 * @param paddingRight Right padding
 * @param paddingTop Top padding
 * @param paddingBottom Bottom padding
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_avgpool2d")
func cswift_mps_graph_avgpool2d(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ kernelWidth: Int32,
    _ kernelHeight: Int32,
    _ strideX: Int32,
    _ strideY: Int32,
    _ paddingLeft: Int32,
    _ paddingRight: Int32,
    _ paddingTop: Int32,
    _ paddingBottom: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let descriptor = MPSGraphPooling2DOpDescriptor(
        kernelWidth: Int(kernelWidth),
        kernelHeight: Int(kernelHeight),
        strideInX: Int(strideX),
        strideInY: Int(strideY),
        dilationRateInX: 1,
        dilationRateInY: 1,
        paddingLeft: Int(paddingLeft),
        paddingRight: Int(paddingRight),
        paddingTop: Int(paddingTop),
        paddingBottom: Int(paddingBottom),
        paddingStyle: .explicit,
        dataLayout: .NHWC
    )
    
    let result = graph.graph.avgPooling2D(
        withSourceTensor: input.tensor,
        descriptor: descriptor!,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: [1, 1, 1, 1],
        dataType: .float32
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Sigmoid activation function
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_sigmoid")
func cswift_mps_graph_sigmoid(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.sigmoid(
        with: input.tensor,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Tanh activation function
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_tanh")
func cswift_mps_graph_tanh(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.tanh(
        with: input.tensor,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Leaky ReLU activation function
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param alpha Negative slope
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_leaky_relu")
func cswift_mps_graph_leaky_relu(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ alpha: Float,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.leakyReLU(
        with: input.tensor,
        alpha: Double(alpha),
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Softmax operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param axis Axis along which to compute softmax
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_softmax")
func cswift_mps_graph_softmax(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ axis: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.softMax(
        with: input.tensor,
        axis: Int(axis),
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Batch normalization operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param meanHandle Running mean tensor
 * @param varianceHandle Running variance tensor
 * @param gammaHandle Scale tensor
 * @param betaHandle Shift tensor
 * @param epsilon Small value to avoid division by zero
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_batch_norm")
func cswift_mps_graph_batch_norm(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ meanHandle: OpaquePointer,
    _ varianceHandle: OpaquePointer,
    _ gammaHandle: OpaquePointer?,
    _ betaHandle: OpaquePointer?,
    _ epsilon: Float,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    let mean = Unmanaged<TensorWrapper>.fromOpaque(meanHandle.toRawPointer()).takeUnretainedValue()
    let variance = Unmanaged<TensorWrapper>.fromOpaque(varianceHandle.toRawPointer()).takeUnretainedValue()
    
    let gamma = gammaHandle != nil ? Unmanaged<TensorWrapper>.fromOpaque(gammaHandle!.toRawPointer()).takeUnretainedValue() : nil
    let beta = betaHandle != nil ? Unmanaged<TensorWrapper>.fromOpaque(betaHandle!.toRawPointer()).takeUnretainedValue() : nil
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.normalize(
        input.tensor,
        mean: mean.tensor,
        variance: variance.tensor,
        gamma: gamma?.tensor,
        beta: beta?.tensor,
        epsilon: epsilon,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Reshape operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param shape New shape array
 * @param shapeCount Number of dimensions
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_reshape")
func cswift_mps_graph_reshape(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ shape: UnsafePointer<Int32>,
    _ shapeCount: Int,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    // Convert shape to NSNumber array
    var shapeArray: [NSNumber] = []
    for i in 0..<shapeCount {
        shapeArray.append(NSNumber(value: shape[i]))
    }
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result = graph.graph.reshape(
        input.tensor,
        shape: shapeArray,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: shapeArray.map { $0.intValue },
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Dropout operation
 * 
 * @param graphHandle Graph handle
 * @param inputHandle Input tensor
 * @param rate Dropout rate (0.0 to 1.0)
 * @param training Whether in training mode
 * @param name Optional operation name
 * @return Result tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_dropout")
func cswift_mps_graph_dropout(
    _ graphHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ rate: Float,
    _ training: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let input = Unmanaged<TensorWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let result: MPSGraphTensor
    if training != 0 {
        result = graph.graph.dropout(
            input.tensor,
            rate: Double(rate),
            name: nameStr
        )
    } else {
        // In inference mode, dropout is a no-op
        result = input.tensor
    }
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: input.tensor.shape,
        dataType: input.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}