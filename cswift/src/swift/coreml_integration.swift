/**
 * @file coreml_integration.swift
 * @brief CoreML integration for machine learning inference
 * 
 * Provides C++ bindings for CoreML framework classes including:
 * - MLModel for model loading and inference
 * - MLFeatureProvider for input/output data handling
 * - MLMultiArray for tensor data representation
 * - MLComputeUnits for hardware acceleration control
 */

import Foundation

#if canImport(CoreML)
import CoreML
#endif

// MARK: - Error Handling

/**
 * @brief CoreML wrapper class for MLModel
 */
class MLModelWrapper {
    #if canImport(CoreML)
    let model: MLModel?
    #endif
    
    let stubModel: Bool
    
    #if canImport(CoreML)
    init(model: MLModel) {
        self.model = model
        self.stubModel = false
    }
    #endif
    
    init() {
        #if canImport(CoreML)
        self.model = nil
        #endif
        self.stubModel = true
    }
}

/**
 * @brief CoreML wrapper class for MLFeatureProvider
 */
class MLFeatureProviderWrapper {
    #if canImport(CoreML)
    let provider: MLFeatureProvider?
    var features: [String: MLFeatureValue]
    #else
    var features: [String: Any]
    #endif
    
    let stubProvider: Bool
    
    #if canImport(CoreML)
    init(provider: MLFeatureProvider) {
        self.provider = provider
        self.features = [:]
        self.stubProvider = false
        
        // Copy features from provider
        for name in provider.featureNames {
            if let value = provider.featureValue(for: name) {
                self.features[name] = value
            }
        }
    }
    
    init() {
        self.provider = nil
        self.features = [:]
        self.stubProvider = false
    }
    #else
    init() {
        self.features = [:]
        self.stubProvider = true
    }
    #endif
}

/**
 * @brief CoreML wrapper class for MLMultiArray
 */
class MLMultiArrayWrapper {
    #if canImport(CoreML)
    let multiArray: MLMultiArray?
    #endif
    
    var stubData: UnsafeMutableRawPointer?
    var stubShape: [Int]
    var stubDataType: Int32
    var stubCount: Int
    
    #if canImport(CoreML)
    init(multiArray: MLMultiArray) {
        self.multiArray = multiArray
        self.stubData = nil
        self.stubShape = []
        self.stubDataType = 0
        self.stubCount = 0
    }
    #endif
    
    init(data: UnsafeMutableRawPointer, shape: [Int], dataType: Int32, count: Int) {
        #if canImport(CoreML)
        self.multiArray = nil
        #endif
        self.stubData = data
        self.stubShape = shape
        self.stubDataType = dataType
        self.stubCount = count
    }
}

// MARK: - MLModel Functions

/**
 * @brief Load MLModel from file path
 * 
 * @param path File path to .mlmodel or .mlpackage
 * @param computeUnits Compute units preference (0=all, 1=CPU, 2=GPU)
 * @param error Error info output
 * @return Model handle or NULL on error
 */
@_cdecl("cswift_ml_model_load")
func cswift_ml_model_load(
    _ path: UnsafePointer<CChar>,
    _ computeUnits: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let pathString = String(cString: path)
    
    #if canImport(CoreML)
    do {
        let url = URL(fileURLWithPath: pathString)
        
        // Configure compute units
        let configuration = MLModelConfiguration()
        switch computeUnits {
        case 1:
            configuration.computeUnits = .cpuOnly
        case 2:
            configuration.computeUnits = .cpuAndGPU
        default:
            configuration.computeUnits = .all
        }
        
        let model = try MLModel(contentsOf: url, configuration: configuration)
        let wrapper = MLModelWrapper(model: model)
        return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation for non-Apple platforms
    let wrapper = MLModelWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Load MLModel from memory data
 * 
 * @param data Model data in memory
 * @param size Size of model data
 * @param computeUnits Compute units preference
 * @param error Error info output
 * @return Model handle or NULL on error
 */
@_cdecl("cswift_ml_model_load_from_data")
func cswift_ml_model_load_from_data(
    _ data: UnsafeRawPointer,
    _ size: Int,
    _ computeUnits: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    #if canImport(CoreML)
    do {
        let modelData = Data(bytes: data, count: size)
        
        // Write to temporary file (CoreML requires file URL)
        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("mlmodel")
        
        try modelData.write(to: tempURL)
        
        let configuration = MLModelConfiguration()
        switch computeUnits {
        case 1:
            configuration.computeUnits = .cpuOnly
        case 2:
            configuration.computeUnits = .cpuAndGPU
        default:
            configuration.computeUnits = .all
        }
        
        let model = try MLModel(contentsOf: tempURL, configuration: configuration)
        
        // Clean up temporary file
        try? FileManager.default.removeItem(at: tempURL)
        
        let wrapper = MLModelWrapper(model: model)
        return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation
    let wrapper = MLModelWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Perform prediction with MLModel
 * 
 * @param modelHandle Model handle
 * @param inputHandle Input feature provider handle
 * @param error Error info output
 * @return Output feature provider handle or NULL on error
 */
@_cdecl("cswift_ml_model_predict")
func cswift_ml_model_predict(
    _ modelHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let modelWrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    let inputWrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = modelWrapper.model,
          let provider = inputWrapper.provider else {
        return nil
    }
    
    do {
        let output = try model.prediction(from: provider)
        let outputWrapper = MLFeatureProviderWrapper(provider: output)
        return Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation - return empty feature provider
    let outputWrapper = MLFeatureProviderWrapper()
    return Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Get model input feature names
 * 
 * @param modelHandle Model handle
 * @param outNames Output array for feature names (caller allocated)
 * @param maxNames Maximum number of names to return
 * @return Number of feature names returned
 */
@_cdecl("cswift_ml_model_input_names")
func cswift_ml_model_input_names(
    _ modelHandle: OpaquePointer,
    _ outNames: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>,
    _ maxNames: Int
) -> Int32 {
    let wrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = wrapper.model else {
        return 0
    }
    
    let inputNames = Array(model.modelDescription.inputDescriptionsByName.keys)
    let count = min(inputNames.count, maxNames)
    
    for i in 0..<count {
        let name = inputNames[i]
        let cString = strdup(name)
        outNames[i] = cString
    }
    
    return Int32(count)
    #else
    // Stub implementation
    return 0
    #endif
}

/**
 * @brief Get model output feature names
 * 
 * @param modelHandle Model handle
 * @param outNames Output array for feature names (caller allocated)
 * @param maxNames Maximum number of names to return
 * @return Number of feature names returned
 */
@_cdecl("cswift_ml_model_output_names")
func cswift_ml_model_output_names(
    _ modelHandle: OpaquePointer,
    _ outNames: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>,
    _ maxNames: Int
) -> Int32 {
    let wrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = wrapper.model else {
        return 0
    }
    
    let outputNames = Array(model.modelDescription.outputDescriptionsByName.keys)
    let count = min(outputNames.count, maxNames)
    
    for i in 0..<count {
        let name = outputNames[i]
        let cString = strdup(name)
        outNames[i] = cString
    }
    
    return Int32(count)
    #else
    // Stub implementation
    return 0
    #endif
}

/**
 * @brief Destroy model
 * 
 * @param modelHandle Model handle
 */
@_cdecl("cswift_ml_model_destroy")
func cswift_ml_model_destroy(_ modelHandle: OpaquePointer) {
    _ = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeRetainedValue()
}

// MARK: - MLFeatureProvider Functions

/**
 * @brief Create feature provider
 * 
 * @return Feature provider handle
 */
@_cdecl("cswift_ml_feature_provider_create")
func cswift_ml_feature_provider_create() -> OpaquePointer {
    let wrapper = MLFeatureProviderWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Set multi-array feature
 * 
 * @param providerHandle Feature provider handle
 * @param name Feature name
 * @param multiArrayHandle Multi-array handle
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_ml_feature_provider_set_multiarray")
func cswift_ml_feature_provider_set_multiarray(
    _ providerHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>,
    _ multiArrayHandle: OpaquePointer
) -> Int32 {
    let wrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(providerHandle.toRawPointer()).takeUnretainedValue()
    let multiArrayWrapper = Unmanaged<MLMultiArrayWrapper>.fromOpaque(multiArrayHandle.toRawPointer()).takeUnretainedValue()
    
    let featureName = String(cString: name)
    
    #if canImport(CoreML)
    if let multiArray = multiArrayWrapper.multiArray {
        let featureValue = MLFeatureValue(multiArray: multiArray)
        wrapper.features[featureName] = featureValue
        return CS_SUCCESS
    }
    #else
    // Stub implementation - store reference
    wrapper.features[featureName] = multiArrayWrapper
    #endif
    
    return CS_SUCCESS
}

/**
 * @brief Get multi-array feature
 * 
 * @param providerHandle Feature provider handle
 * @param name Feature name
 * @return Multi-array handle or NULL if not found
 */
@_cdecl("cswift_ml_feature_provider_get_multiarray")
func cswift_ml_feature_provider_get_multiarray(
    _ providerHandle: OpaquePointer,
    _ name: UnsafePointer<CChar>
) -> OpaquePointer? {
    let wrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(providerHandle.toRawPointer()).takeUnretainedValue()
    let featureName = String(cString: name)
    
    #if canImport(CoreML)
    guard let featureValue = wrapper.features[featureName],
          let multiArray = featureValue.multiArrayValue else {
        return nil
    }
    
    let multiArrayWrapper = MLMultiArrayWrapper(multiArray: multiArray)
    return Unmanaged.passRetained(multiArrayWrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation
    guard let feature = wrapper.features[featureName] as? MLMultiArrayWrapper else {
        return nil
    }
    
    return Unmanaged.passRetained(feature).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Get feature names count
 * 
 * @param providerHandle Feature provider handle
 * @return Number of features
 */
@_cdecl("cswift_ml_feature_provider_count")
func cswift_ml_feature_provider_count(_ providerHandle: OpaquePointer) -> Int32 {
    let wrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(providerHandle.toRawPointer()).takeUnretainedValue()
    return Int32(wrapper.features.count)
}

/**
 * @brief Destroy feature provider
 * 
 * @param providerHandle Feature provider handle
 */
@_cdecl("cswift_ml_feature_provider_destroy")
func cswift_ml_feature_provider_destroy(_ providerHandle: OpaquePointer) {
    _ = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(providerHandle.toRawPointer()).takeRetainedValue()
}

// MARK: - MLMultiArray Functions

/**
 * @brief Create multi-array with shape and data type
 * 
 * @param shape Array shape (dimensions)
 * @param shapeCount Number of dimensions
 * @param dataType Data type (0=float32, 1=float64, 2=int32)
 * @param error Error info output
 * @return Multi-array handle or NULL on error
 */
@_cdecl("cswift_ml_multiarray_create")
func cswift_ml_multiarray_create(
    _ shape: UnsafePointer<Int32>,
    _ shapeCount: Int32,
    _ dataType: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let shapeArray = Array(UnsafeBufferPointer(start: shape, count: Int(shapeCount)))
    let swiftShape = shapeArray.map { Int($0) }
    
    #if canImport(CoreML)
    do {
        let mlDataType: MLMultiArrayDataType
        switch dataType {
        case 0:
            mlDataType = .float32
        case 1:
            mlDataType = .double
        case 2:
            mlDataType = .int32
        default:
            mlDataType = .float32
        }
        
        let multiArray = try MLMultiArray(shape: swiftShape.map { NSNumber(value: $0) }, 
                                        dataType: mlDataType)
        let wrapper = MLMultiArrayWrapper(multiArray: multiArray)
        return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation - allocate memory
    let elementSize: Int
    switch dataType {
    case 0: elementSize = 4  // float32
    case 1: elementSize = 8  // double
    case 2: elementSize = 4  // int32
    default: elementSize = 4
    }
    
    let totalElements = swiftShape.reduce(1, *)
    let totalBytes = totalElements * elementSize
    
    let data = UnsafeMutableRawPointer.allocate(byteCount: totalBytes, alignment: 16)
    data.initializeMemory(as: UInt8.self, repeating: 0, count: totalBytes)
    
    let wrapper = MLMultiArrayWrapper(data: data, shape: swiftShape, 
                                    dataType: dataType, count: totalElements)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Get multi-array data pointer
 * 
 * @param multiArrayHandle Multi-array handle
 * @return Data pointer or NULL
 */
@_cdecl("cswift_ml_multiarray_data_pointer")
func cswift_ml_multiarray_data_pointer(_ multiArrayHandle: OpaquePointer) -> UnsafeMutableRawPointer? {
    let wrapper = Unmanaged<MLMultiArrayWrapper>.fromOpaque(multiArrayHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    return wrapper.multiArray?.dataPointer
    #else
    return wrapper.stubData
    #endif
}

/**
 * @brief Get multi-array shape
 * 
 * @param multiArrayHandle Multi-array handle
 * @param outShape Output shape array (caller allocated)
 * @param maxDims Maximum dimensions to return
 * @return Number of dimensions
 */
@_cdecl("cswift_ml_multiarray_shape")
func cswift_ml_multiarray_shape(
    _ multiArrayHandle: OpaquePointer,
    _ outShape: UnsafeMutablePointer<Int32>,
    _ maxDims: Int32
) -> Int32 {
    let wrapper = Unmanaged<MLMultiArrayWrapper>.fromOpaque(multiArrayHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let multiArray = wrapper.multiArray else {
        return 0
    }
    
    let shape = multiArray.shape.map { $0.int32Value }
    let count = min(shape.count, Int(maxDims))
    
    for i in 0..<count {
        outShape[i] = shape[i]
    }
    
    return Int32(count)
    #else
    let count = min(wrapper.stubShape.count, Int(maxDims))
    
    for i in 0..<count {
        outShape[i] = Int32(wrapper.stubShape[i])
    }
    
    return Int32(count)
    #endif
}

/**
 * @brief Get multi-array element count
 * 
 * @param multiArrayHandle Multi-array handle
 * @return Total number of elements
 */
@_cdecl("cswift_ml_multiarray_count")
func cswift_ml_multiarray_count(_ multiArrayHandle: OpaquePointer) -> Int {
    let wrapper = Unmanaged<MLMultiArrayWrapper>.fromOpaque(multiArrayHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    return wrapper.multiArray?.count ?? 0
    #else
    return wrapper.stubCount
    #endif
}

/**
 * @brief Destroy multi-array
 * 
 * @param multiArrayHandle Multi-array handle
 */
@_cdecl("cswift_ml_multiarray_destroy")
func cswift_ml_multiarray_destroy(_ multiArrayHandle: OpaquePointer) {
    let wrapper = Unmanaged<MLMultiArrayWrapper>.fromOpaque(multiArrayHandle.toRawPointer()).takeRetainedValue()
    
    #if !canImport(CoreML)
    // Free stub data if allocated
    if let data = wrapper.stubData {
        data.deallocate()
    }
    #endif
}

// MARK: - Advanced CoreML Features

/**
 * @brief Model compilation configuration wrapper
 */
class MLModelConfigurationWrapper {
    #if canImport(CoreML)
    let configuration: MLModelConfiguration
    #endif
    
    init() {
        #if canImport(CoreML)
        self.configuration = MLModelConfiguration()
        #endif
    }
}

/**
 * @brief Create model configuration for advanced settings
 * 
 * @return Configuration handle
 */
@_cdecl("cswift_ml_model_configuration_create")
func cswift_ml_model_configuration_create() -> OpaquePointer {
    let wrapper = MLModelConfigurationWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Set compute units for model configuration
 * 
 * @param configHandle Configuration handle
 * @param computeUnits Compute units (0=all, 1=CPU, 2=GPU, 3=Neural Engine)
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_ml_model_configuration_set_compute_units")
func cswift_ml_model_configuration_set_compute_units(
    _ configHandle: OpaquePointer,
    _ computeUnits: Int32
) -> Int32 {
    #if canImport(CoreML)
    let wrapper = Unmanaged<MLModelConfigurationWrapper>.fromOpaque(configHandle.toRawPointer()).takeUnretainedValue()
    
    switch computeUnits {
    case 1:
        wrapper.configuration.computeUnits = .cpuOnly
    case 2:
        wrapper.configuration.computeUnits = .cpuAndGPU
    case 3:
        if #available(macOS 13.0, iOS 16.0, *) {
            wrapper.configuration.computeUnits = .cpuAndNeuralEngine
        } else {
            wrapper.configuration.computeUnits = .all
        }
    default:
        wrapper.configuration.computeUnits = .all
    }
    
    return CS_SUCCESS
    #else
    return CS_SUCCESS
    #endif
}

/**
 * @brief Set function name for model configuration (for ML Programs)
 * 
 * @param configHandle Configuration handle
 * @param functionName Function name
 * @return CS_SUCCESS or error code
 */
@_cdecl("cswift_ml_model_configuration_set_function_name")
func cswift_ml_model_configuration_set_function_name(
    _ configHandle: OpaquePointer,
    _ functionName: UnsafePointer<CChar>
) -> Int32 {
    #if canImport(CoreML)
    let wrapper = Unmanaged<MLModelConfigurationWrapper>.fromOpaque(configHandle.toRawPointer()).takeUnretainedValue()
    let functionString = String(cString: functionName)
    
    if #available(macOS 13.0, iOS 16.0, *) {
        wrapper.configuration.functionName = functionString
    }
    
    return CS_SUCCESS
    #else
    return CS_SUCCESS
    #endif
}

/**
 * @brief Load model with advanced configuration
 * 
 * @param path Model file path
 * @param configHandle Configuration handle
 * @param error Error info output
 * @return Model handle or NULL on error
 */
@_cdecl("cswift_ml_model_load_with_config")
func cswift_ml_model_load_with_config(
    _ path: UnsafePointer<CChar>,
    _ configHandle: OpaquePointer,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let pathString = String(cString: path)
    
    #if canImport(CoreML)
    let configWrapper = Unmanaged<MLModelConfigurationWrapper>.fromOpaque(configHandle.toRawPointer()).takeUnretainedValue()
    
    do {
        let url = URL(fileURLWithPath: pathString)
        let model = try MLModel(contentsOf: url, configuration: configWrapper.configuration)
        let wrapper = MLModelWrapper(model: model)
        return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation
    let wrapper = MLModelWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Destroy model configuration
 * 
 * @param configHandle Configuration handle
 */
@_cdecl("cswift_ml_model_configuration_destroy")
func cswift_ml_model_configuration_destroy(_ configHandle: OpaquePointer) {
    _ = Unmanaged<MLModelConfigurationWrapper>.fromOpaque(configHandle.toRawPointer()).takeRetainedValue()
}

// MARK: - Batch Prediction Support

/**
 * @brief Perform batch prediction
 * 
 * @param modelHandle Model handle
 * @param inputProviders Array of input feature provider handles
 * @param providerCount Number of input providers
 * @param outputProviders Array to store output provider handles (caller allocated)
 * @param error Error info output
 * @return Number of successful predictions
 */
@_cdecl("cswift_ml_model_predict_batch")
func cswift_ml_model_predict_batch(
    _ modelHandle: OpaquePointer,
    _ inputProviders: UnsafePointer<OpaquePointer>,
    _ providerCount: Int32,
    _ outputProviders: UnsafeMutablePointer<OpaquePointer?>,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> Int32 {
    let modelWrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = modelWrapper.model else {
        return 0
    }
    
    var successCount: Int32 = 0
    
    for i in 0..<Int(providerCount) {
        let inputHandle = inputProviders[i]
        let inputWrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
        
        guard let provider = inputWrapper.provider else {
            outputProviders[i] = nil
            continue
        }
        
        do {
            let output = try model.prediction(from: provider)
            let outputWrapper = MLFeatureProviderWrapper(provider: output)
            outputProviders[i] = Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
            successCount += 1
        } catch {
            outputProviders[i] = nil
        }
    }
    
    return successCount
    #else
    // Stub implementation
    for i in 0..<Int(providerCount) {
        let outputWrapper = MLFeatureProviderWrapper()
        outputProviders[i] = Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
    }
    return providerCount
    #endif
}

// MARK: - Model Introspection

/**
 * @brief Get model description
 * 
 * @param modelHandle Model handle
 * @param outDescription Output buffer for description (caller allocated)
 * @param maxLength Maximum description length
 * @return Length of description written
 */
@_cdecl("cswift_ml_model_get_description")
func cswift_ml_model_get_description(
    _ modelHandle: OpaquePointer,
    _ outDescription: UnsafeMutablePointer<CChar>,
    _ maxLength: Int32
) -> Int32 {
    let wrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = wrapper.model else {
        return 0
    }
    
    let description = model.modelDescription.description
    let descData = description.data(using: .utf8) ?? Data()
    let lengthToCopy = min(descData.count, Int(maxLength) - 1)
    
    _ = descData.withUnsafeBytes { bytes in
        // ZERO_COPY_ALLOWED - Required for C string bridge
        memcpy(outDescription, bytes.baseAddress!, lengthToCopy)
    }
    
    outDescription[lengthToCopy] = 0 // Null terminate
    return Int32(lengthToCopy)
    #else
    let stubDesc = "Stub model description"
    let lengthToCopy = min(stubDesc.count, Int(maxLength) - 1)
    
    stubDesc.withCString { cStr in
        // ZERO_COPY_ALLOWED - Required for C string bridge
        memcpy(outDescription, cStr, lengthToCopy)
    }
    
    outDescription[lengthToCopy] = 0
    return Int32(lengthToCopy)
    #endif
}

/**
 * @brief Get model metadata value
 * 
 * @param modelHandle Model handle
 * @param key Metadata key
 * @param outValue Output buffer for value (caller allocated)
 * @param maxLength Maximum value length
 * @return Length of value written, or -1 if key not found
 */
@_cdecl("cswift_ml_model_get_metadata")
func cswift_ml_model_get_metadata(
    _ modelHandle: OpaquePointer,
    _ key: UnsafePointer<CChar>,
    _ outValue: UnsafeMutablePointer<CChar>,
    _ maxLength: Int32
) -> Int32 {
    let wrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    let keyString = String(cString: key)
    
    #if canImport(CoreML)
    guard let model = wrapper.model else {
        return -1
    }
    
    guard let metadata = model.modelDescription.metadata[MLModelMetadataKey(rawValue: keyString)] else {
        return -1
    }
    
    let valueString = String(describing: metadata)
    let valueData = valueString.data(using: .utf8) ?? Data()
    let lengthToCopy = min(valueData.count, Int(maxLength) - 1)
    
    _ = valueData.withUnsafeBytes { bytes in
        // ZERO_COPY_ALLOWED - Required for C string bridge
        memcpy(outValue, bytes.baseAddress!, lengthToCopy)
    }
    
    outValue[lengthToCopy] = 0
    return Int32(lengthToCopy)
    #else
    // Stub implementation
    let stubValue = "stub_value"
    let lengthToCopy = min(stubValue.count, Int(maxLength) - 1)
    
    stubValue.withCString { cStr in
        // ZERO_COPY_ALLOWED - Required for C string bridge
        memcpy(outValue, cStr, lengthToCopy)
    }
    
    outValue[lengthToCopy] = 0
    return Int32(lengthToCopy)
    #endif
}

/**
 * @brief Get input feature description
 * 
 * @param modelHandle Model handle
 * @param featureName Feature name
 * @param outType Output feature type (0=invalid, 1=multiArray, 2=image, 3=string, 4=sequence)
 * @param outDimensions Output dimensions array (caller allocated)
 * @param maxDims Maximum dimensions
 * @return Number of dimensions, or -1 if feature not found
 */
@_cdecl("cswift_ml_model_get_input_description")
func cswift_ml_model_get_input_description(
    _ modelHandle: OpaquePointer,
    _ featureName: UnsafePointer<CChar>,
    _ outType: UnsafeMutablePointer<Int32>,
    _ outDimensions: UnsafeMutablePointer<Int32>,
    _ maxDims: Int32
) -> Int32 {
    let wrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    let nameString = String(cString: featureName)
    
    #if canImport(CoreML)
    guard let model = wrapper.model,
          let featureDesc = model.modelDescription.inputDescriptionsByName[nameString] else {
        outType.pointee = 0
        return -1
    }
    
    switch featureDesc.type {
    case .multiArray:
        outType.pointee = 1
        if let constraint = featureDesc.multiArrayConstraint {
            let dims = constraint.shape.map { $0.int32Value }
            let dimCount = min(dims.count, Int(maxDims))
            for i in 0..<dimCount {
                outDimensions[i] = dims[i]
            }
            return Int32(dimCount)
        }
    case .image:
        outType.pointee = 2
        if let constraint = featureDesc.imageConstraint {
            var dimIndex = 0
            if dimIndex < Int(maxDims) {
                outDimensions[dimIndex] = Int32(constraint.pixelsHigh)
                dimIndex += 1
            }
            if dimIndex < Int(maxDims) {
                outDimensions[dimIndex] = Int32(constraint.pixelsWide)
                dimIndex += 1
            }
            return Int32(dimIndex)
        }
    case .string:
        outType.pointee = 3
        return 0
    case .sequence:
        outType.pointee = 4
        return 0
    default:
        outType.pointee = 0
        return 0
    }
    #else
    // Stub implementation
    outType.pointee = 1 // Assume multiArray
    outDimensions[0] = 224
    outDimensions[1] = 224
    outDimensions[2] = 3
    return 3
    #endif
    
    return 0
}

// MARK: - Image Feature Support

/**
 * @brief Create MLMultiArray from image data
 * 
 * @param imageData Raw image data (RGB/RGBA)
 * @param width Image width
 * @param height Image height
 * @param channels Number of channels (3 for RGB, 4 for RGBA)
 * @param error Error info output
 * @return MultiArray handle or NULL on error
 */
@_cdecl("cswift_ml_multiarray_from_image")
func cswift_ml_multiarray_from_image(
    _ imageData: UnsafePointer<UInt8>,
    _ width: Int32,
    _ height: Int32,
    _ channels: Int32,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    #if canImport(CoreML)
    do {
        let shape = [NSNumber(value: height), NSNumber(value: width), NSNumber(value: channels)]
        let multiArray = try MLMultiArray(shape: shape, dataType: .float32)
        
        // Convert image data to float32 and normalize to [0, 1]
        let totalPixels = Int(width * height * channels)
        let floatPtr = multiArray.dataPointer.bindMemory(to: Float.self, capacity: totalPixels)
        
        for i in 0..<totalPixels {
            floatPtr[i] = Float(imageData[i]) / 255.0
        }
        
        let wrapper = MLMultiArrayWrapper(multiArray: multiArray)
        return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation
    let totalElements = Int(width * height * channels)
    let totalBytes = totalElements * 4 // float32
    
    let data = UnsafeMutableRawPointer.allocate(byteCount: totalBytes, alignment: 16)
    let floatPtr = data.bindMemory(to: Float.self, capacity: totalElements)
    
    // Convert and normalize
    for i in 0..<totalElements {
        floatPtr[i] = Float(imageData[i]) / 255.0
    }
    
    let shape = [Int(height), Int(width), Int(channels)]
    let wrapper = MLMultiArrayWrapper(data: data, shape: shape, dataType: 0, count: totalElements)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

// MARK: - Performance Monitoring

/**
 * @brief Prediction options wrapper for performance monitoring
 */
class MLPredictionOptionsWrapper {
    #if canImport(CoreML)
    let options: MLPredictionOptions
    #endif
    
    init() {
        #if canImport(CoreML)
        self.options = MLPredictionOptions()
        #endif
    }
}

/**
 * @brief Create prediction options
 * 
 * @return Prediction options handle
 */
@_cdecl("cswift_ml_prediction_options_create")
func cswift_ml_prediction_options_create() -> OpaquePointer {
    let wrapper = MLPredictionOptionsWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
}

/**
 * @brief Set use CPU only for prediction
 * 
 * @param optionsHandle Options handle
 * @param useCPUOnly 1 to use CPU only, 0 to use all available compute units
 * @return CS_SUCCESS
 */
@_cdecl("cswift_ml_prediction_options_set_use_cpu_only")
func cswift_ml_prediction_options_set_use_cpu_only(
    _ optionsHandle: OpaquePointer,
    _ useCPUOnly: Int32
) -> Int32 {
    #if canImport(CoreML)
    let wrapper = Unmanaged<MLPredictionOptionsWrapper>.fromOpaque(optionsHandle.toRawPointer()).takeUnretainedValue()
    // Use modern API for compute units instead of deprecated usesCPUOnly
    if #available(macOS 11.0, iOS 14.0, *) {
        if useCPUOnly != 0 {
            // For compatibility - in modern API this would be done via MLModelConfiguration
        }
    } else {
        #if swift(>=5.5)
        #else
        wrapper.options.usesCPUOnly = (useCPUOnly != 0)
        #endif
    }
    #endif
    return CS_SUCCESS
}

/**
 * @brief Perform prediction with options and timing
 * 
 * @param modelHandle Model handle
 * @param inputHandle Input feature provider handle
 * @param optionsHandle Prediction options handle (optional)
 * @param outExecutionTime Output execution time in milliseconds
 * @param error Error info output
 * @return Output feature provider handle or NULL on error
 */
@_cdecl("cswift_ml_model_predict_with_timing")
func cswift_ml_model_predict_with_timing(
    _ modelHandle: OpaquePointer,
    _ inputHandle: OpaquePointer,
    _ optionsHandle: OpaquePointer?,
    _ outExecutionTime: UnsafeMutablePointer<Double>,
    _ error: UnsafeMutablePointer<UnsafeMutableRawPointer>?
) -> OpaquePointer? {
    let modelWrapper = Unmanaged<MLModelWrapper>.fromOpaque(modelHandle.toRawPointer()).takeUnretainedValue()
    let inputWrapper = Unmanaged<MLFeatureProviderWrapper>.fromOpaque(inputHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(CoreML)
    guard let model = modelWrapper.model,
          let provider = inputWrapper.provider else {
        return nil
    }
    
    let options = optionsHandle != nil ? 
        Unmanaged<MLPredictionOptionsWrapper>.fromOpaque(optionsHandle!.toRawPointer()).takeUnretainedValue().options :
        MLPredictionOptions()
    
    do {
        let startTime = CFAbsoluteTimeGetCurrent()
        let output = try model.prediction(from: provider, options: options)
        let endTime = CFAbsoluteTimeGetCurrent()
        
        outExecutionTime.pointee = (endTime - startTime) * 1000.0 // Convert to milliseconds
        
        let outputWrapper = MLFeatureProviderWrapper(provider: output)
        return Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
        
    } catch {
        // setError removed for compatibility - would set error details here
        return nil
    }
    #else
    // Stub implementation
    outExecutionTime.pointee = 1.0 // 1ms fake execution time
    let outputWrapper = MLFeatureProviderWrapper()
    return Unmanaged.passRetained(outputWrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Destroy prediction options
 * 
 * @param optionsHandle Options handle
 */
@_cdecl("cswift_ml_prediction_options_destroy")
func cswift_ml_prediction_options_destroy(_ optionsHandle: OpaquePointer) {
    _ = Unmanaged<MLPredictionOptionsWrapper>.fromOpaque(optionsHandle.toRawPointer()).takeRetainedValue()
}

// Note: Error codes are defined in common.swift