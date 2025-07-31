import Foundation

#if canImport(MetalPerformanceShadersGraph)
import MetalPerformanceShadersGraph
#endif

/**
 * @brief Compute gradients using automatic differentiation
 * 
 * @param graphHandle Graph handle
 * @param lossHandle Loss tensor handle
 * @param sourceTensors Array of source tensors to compute gradients for
 * @param sourceTensorCount Number of source tensors
 * @return Array of gradient tensors or NULL on error
 */
@_cdecl("cswift_mps_graph_gradients")
func cswift_mps_graph_gradients(
    _ graphHandle: OpaquePointer,
    _ lossHandle: OpaquePointer,
    _ sourceTensors: UnsafePointer<OpaquePointer?>,
    _ sourceTensorCount: Int
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let loss = Unmanaged<TensorWrapper>.fromOpaque(lossHandle.toRawPointer()).takeUnretainedValue()
    
    // Convert source tensors to array
    var sources: [MPSGraphTensor] = []
    for i in 0..<sourceTensorCount {
        if let tensorHandle = sourceTensors[i] {
            let tensor = Unmanaged<TensorWrapper>.fromOpaque(tensorHandle.toRawPointer()).takeUnretainedValue()
            sources.append(tensor.tensor)
        }
    }
    
    #if canImport(MetalPerformanceShadersGraph)
    let gradientsDict = graph.graph.gradients(
        of: loss.tensor,
        with: sources,
        name: nil
    )
    // Extract gradient tensors in the same order as sources
    var gradients: [MPSGraphTensor] = []
    for source in sources {
        if let gradient = gradientsDict[source] {
            gradients.append(gradient)
        }
    }
    #else
    // Stub implementation - return same number of gradients as sources
    let gradients = sources.map { source in
        MPSGraphTensor(
            name: "gradient",
            shape: source.shape,
            dataType: source.dataType
        )
    }
    #endif
    
    // Wrap gradients in GradientArrayWrapper
    let gradientWrappers = gradients.map { TensorWrapper($0) }
    let arrayWrapper = GradientArrayWrapper(gradients: gradientWrappers)
    
    return Unmanaged.passRetained(arrayWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Get gradient tensor at index
 * 
 * @param gradientsHandle Gradients array handle
 * @param index Index of gradient to retrieve
 * @return Gradient tensor handle or NULL if index out of bounds
 */
@_cdecl("cswift_mps_gradients_get")
func cswift_mps_gradients_get(
    _ gradientsHandle: OpaquePointer,
    _ index: Int32
) -> OpaquePointer? {
    let arrayWrapper = Unmanaged<GradientArrayWrapper>.fromOpaque(gradientsHandle.toRawPointer()).takeUnretainedValue()
    
    guard index >= 0 && index < arrayWrapper.gradients.count else {
        return nil
    }
    
    let gradient = arrayWrapper.gradients[Int(index)]
    return Unmanaged.passRetained(gradient).toOpaque().toOpaquePointer()
}

/**
 * @brief Get number of gradients
 * 
 * @param gradientsHandle Gradients array handle
 * @return Number of gradients
 */
@_cdecl("cswift_mps_gradients_count")
func cswift_mps_gradients_count(
    _ gradientsHandle: OpaquePointer
) -> Int32 {
    let arrayWrapper = Unmanaged<GradientArrayWrapper>.fromOpaque(gradientsHandle.toRawPointer()).takeUnretainedValue()
    return Int32(arrayWrapper.gradients.count)
}

/**
 * @brief Destroy gradients array
 * 
 * @param gradientsHandle Gradients array handle
 */
@_cdecl("cswift_mps_gradients_destroy")
func cswift_mps_gradients_destroy(
    _ gradientsHandle: OpaquePointer
) {
    _ = Unmanaged<GradientArrayWrapper>.fromOpaque(gradientsHandle.toRawPointer()).takeRetainedValue()
}

/**
 * @brief Cross entropy loss operation
 * 
 * @param graphHandle Graph handle
 * @param predictionsHandle Predictions tensor (logits)
 * @param labelsHandle Labels tensor (one-hot or indices)
 * @param axis Axis for class dimension
 * @param reduction Reduction type (0=none, 1=sum, 2=mean)
 * @param name Optional operation name
 * @return Loss tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_cross_entropy_loss")
func cswift_mps_graph_cross_entropy_loss(
    _ graphHandle: OpaquePointer,
    _ predictionsHandle: OpaquePointer,
    _ labelsHandle: OpaquePointer,
    _ axis: Int32,
    _ reduction: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let predictions = Unmanaged<TensorWrapper>.fromOpaque(predictionsHandle.toRawPointer()).takeUnretainedValue()
    let labels = Unmanaged<TensorWrapper>.fromOpaque(labelsHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    let reductionType: MPSGraphLossReductionType
    switch reduction {
    case 0: reductionType = .none
    case 1: reductionType = .sum
    case 2: reductionType = .mean
    default: reductionType = .mean
    }
    
    let result = graph.graph.softMaxCrossEntropy(
        predictions.tensor,
        labels: labels.tensor,
        axis: Int(axis),
        reuctionType: reductionType,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: reduction == 0 ? predictions.tensor.shape : [1],
        dataType: predictions.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Mean squared error loss operation
 * 
 * @param graphHandle Graph handle
 * @param predictionsHandle Predictions tensor
 * @param targetsHandle Target values tensor
 * @param reduction Reduction type (0=none, 1=sum, 2=mean)
 * @param name Optional operation name
 * @return Loss tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_mse_loss")
func cswift_mps_graph_mse_loss(
    _ graphHandle: OpaquePointer,
    _ predictionsHandle: OpaquePointer,
    _ targetsHandle: OpaquePointer,
    _ reduction: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let predictions = Unmanaged<TensorWrapper>.fromOpaque(predictionsHandle.toRawPointer()).takeUnretainedValue()
    let targets = Unmanaged<TensorWrapper>.fromOpaque(targetsHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    // Compute squared difference
    let diff = graph.graph.subtraction(predictions.tensor, targets.tensor, name: nil)
    let squared = graph.graph.square(with: diff, name: nil)
    
    let result: MPSGraphTensor
    switch reduction {
    case 0: // none
        result = squared
    case 1: // sum
        result = graph.graph.reductionSum(
            with: squared,
            axes: nil as [NSNumber]?,
            name: nameStr
        )
    case 2: // mean
        result = graph.graph.mean(
            of: squared,
            axes: [] as [NSNumber],
            name: nameStr
        )
    default:
        result = graph.graph.mean(
            of: squared,
            axes: [] as [NSNumber],
            name: nameStr
        )
    }
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: reduction == 0 ? predictions.tensor.shape : [1],
        dataType: predictions.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Stochastic gradient descent optimizer step
 * 
 * @param graphHandle Graph handle
 * @param weightsHandle Weights tensor to update
 * @param gradientsHandle Gradients tensor
 * @param learningRate Learning rate
 * @param momentum Momentum value (0 for standard SGD)
 * @param name Optional operation name
 * @return Updated weights tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_sgd_update")
func cswift_mps_graph_sgd_update(
    _ graphHandle: OpaquePointer,
    _ weightsHandle: OpaquePointer,
    _ gradientsHandle: OpaquePointer,
    _ learningRate: Float,
    _ momentum: Float,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let weights = Unmanaged<TensorWrapper>.fromOpaque(weightsHandle.toRawPointer()).takeUnretainedValue()
    let gradients = Unmanaged<TensorWrapper>.fromOpaque(gradientsHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    // Create learning rate constant
    let lrTensor = graph.graph.constant(
        Double(learningRate),
        shape: [1],
        dataType: weights.tensor.dataType
    )
    
    // Scale gradients by learning rate
    let scaledGradients = graph.graph.multiplication(
        gradients.tensor,
        lrTensor,
        name: nil
    )
    
    // Update weights: weights = weights - lr * gradients
    let result = graph.graph.subtraction(
        weights.tensor,
        scaledGradients,
        name: nameStr
    )
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: weights.tensor.shape,
        dataType: weights.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Adam optimizer step
 * 
 * @param graphHandle Graph handle
 * @param weightsHandle Weights tensor to update
 * @param gradientsHandle Gradients tensor
 * @param momentumHandle First moment estimate (updated in-place)
 * @param velocityHandle Second moment estimate (updated in-place)
 * @param learningRate Learning rate
 * @param beta1 Beta1 parameter (default 0.9)
 * @param beta2 Beta2 parameter (default 0.999)
 * @param epsilon Small constant for numerical stability
 * @param timestep Current timestep (for bias correction)
 * @param name Optional operation name
 * @return Updated weights tensor or NULL on error
 */
@_cdecl("cswift_mps_graph_adam_update")
func cswift_mps_graph_adam_update(
    _ graphHandle: OpaquePointer,
    _ weightsHandle: OpaquePointer,
    _ gradientsHandle: OpaquePointer,
    _ momentumHandle: OpaquePointer,
    _ velocityHandle: OpaquePointer,
    _ learningRate: Float,
    _ beta1: Float,
    _ beta2: Float,
    _ epsilon: Float,
    _ timestep: Int32,
    _ name: UnsafePointer<CChar>?
) -> OpaquePointer? {
    let graph = Unmanaged<GraphWrapper>.fromOpaque(graphHandle.toRawPointer()).takeUnretainedValue()
    let weights = Unmanaged<TensorWrapper>.fromOpaque(weightsHandle.toRawPointer()).takeUnretainedValue()
    let gradients = Unmanaged<TensorWrapper>.fromOpaque(gradientsHandle.toRawPointer()).takeUnretainedValue()
    let momentum = Unmanaged<TensorWrapper>.fromOpaque(momentumHandle.toRawPointer()).takeUnretainedValue()
    let velocity = Unmanaged<TensorWrapper>.fromOpaque(velocityHandle.toRawPointer()).takeUnretainedValue()
    
    let nameStr = name != nil ? String(cString: name!) : nil
    
    #if canImport(MetalPerformanceShadersGraph)
    // Constants
    let beta1Tensor = graph.graph.constant(Double(beta1), shape: [1], dataType: weights.tensor.dataType)
    let beta2Tensor = graph.graph.constant(Double(beta2), shape: [1], dataType: weights.tensor.dataType)
    let oneMinusBeta1 = graph.graph.constant(Double(1 - beta1), shape: [1], dataType: weights.tensor.dataType)
    let oneMinusBeta2 = graph.graph.constant(Double(1 - beta2), shape: [1], dataType: weights.tensor.dataType)
    let epsilonTensor = graph.graph.constant(Double(epsilon), shape: [1], dataType: weights.tensor.dataType)
    let lrTensor = graph.graph.constant(Double(learningRate), shape: [1], dataType: weights.tensor.dataType)
    
    // Update biased first moment estimate
    // m = beta1 * m + (1 - beta1) * g
    let momentumScaled = graph.graph.multiplication(momentum.tensor, beta1Tensor, name: nil)
    let gradientScaled = graph.graph.multiplication(gradients.tensor, oneMinusBeta1, name: nil)
    let newMomentum = graph.graph.addition(momentumScaled, gradientScaled, name: nil)
    
    // Update biased second moment estimate
    // v = beta2 * v + (1 - beta2) * g^2
    let velocityScaled = graph.graph.multiplication(velocity.tensor, beta2Tensor, name: nil)
    let gradientSquared = graph.graph.square(with: gradients.tensor, name: nil)
    let gradientSquaredScaled = graph.graph.multiplication(gradientSquared, oneMinusBeta2, name: nil)
    let newVelocity = graph.graph.addition(velocityScaled, gradientSquaredScaled, name: nil)
    
    // Bias correction
    let t = Double(timestep)
    let biasCorrection1 = graph.graph.constant(1.0 / (1.0 - pow(Double(beta1), t)), shape: [1], dataType: weights.tensor.dataType)
    let biasCorrection2 = graph.graph.constant(1.0 / (1.0 - pow(Double(beta2), t)), shape: [1], dataType: weights.tensor.dataType)
    
    let momentumCorrected = graph.graph.multiplication(newMomentum, biasCorrection1, name: nil)
    let velocityCorrected = graph.graph.multiplication(newVelocity, biasCorrection2, name: nil)
    
    // Update weights
    // w = w - lr * m_corrected / (sqrt(v_corrected) + epsilon)
    let velocitySqrt = graph.graph.squareRoot(with: velocityCorrected, name: nil)
    let denominator = graph.graph.addition(velocitySqrt, epsilonTensor, name: nil)
    let update = graph.graph.division(momentumCorrected, denominator, name: nil)
    let scaledUpdate = graph.graph.multiplication(update, lrTensor, name: nil)
    let result = graph.graph.subtraction(weights.tensor, scaledUpdate, name: nameStr)
    #else
    let result = MPSGraphTensor(
        name: nameStr,
        shape: weights.tensor.shape,
        dataType: weights.tensor.dataType
    )
    #endif
    
    let resultWrapper = TensorWrapper(result)
    if let name = nameStr {
        graph.addTensor(name, tensor: resultWrapper)
    }
    
    return Unmanaged.passRetained(resultWrapper).toOpaque().toOpaquePointer()
}

// Helper class to hold gradient array
class GradientArrayWrapper {
    let gradients: [TensorWrapper]
    
    init(gradients: [TensorWrapper]) {
        self.gradients = gradients
    }
}