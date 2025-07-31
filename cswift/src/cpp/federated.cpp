#include <cswift/detail/federated.hpp>

namespace cswift {

/**
 * @brief Create secure federated learning aggregator
 * 
 * @param privacyLevel Privacy protection level
 * @param encryptionMode Encryption mode for gradient protection
 * @param noiseType Differential privacy noise type
 * @param epsilonPrivacy Privacy budget epsilon (smaller = more private)
 * @param deltaPrivacy Privacy budget delta (smaller = more private)
 */
CSSecureFederatedLearning::CSSecureFederatedLearning(CSPrivacyLevel privacyLevel,
        CSEncryptionMode encryptionMode, CSDifferentialPrivacyNoise noiseType,
        float epsilonPrivacy, float deltaPrivacy)
    : privacyLevel_(privacyLevel)
    , encryptionMode_(encryptionMode)
    , noiseType_(noiseType)
    , epsilonPrivacy_(epsilonPrivacy)
    , deltaPrivacy_(deltaPrivacy) {
    
    void* error = nullptr;
    handle_ = cswift_secure_aggregator_create(
        static_cast<int32_t>(privacyLevel),
        static_cast<int32_t>(encryptionMode),
        static_cast<int32_t>(noiseType),
        epsilonPrivacy,
        deltaPrivacy,
        &error
    );
    
    if (!handle_) {
        std::string errorMsg = "Failed to create secure aggregator";
        if (error) {
            errorMsg += ": " + std::string(static_cast<char*>(error));
            free(error);
        }
        throw CSException(CS_ERROR_OPERATION_FAILED, errorMsg);
    }
}

CSSecureFederatedLearning::~CSSecureFederatedLearning() {
    if (handle_) {
        cswift_secure_aggregator_destroy(handle_);
    }
}

// Move constructor and assignment
CSSecureFederatedLearning::CSSecureFederatedLearning(CSSecureFederatedLearning&& other) noexcept 
    : handle_(other.handle_)
    , privacyLevel_(other.privacyLevel_)
    , encryptionMode_(other.encryptionMode_)
    , noiseType_(other.noiseType_)
    , epsilonPrivacy_(other.epsilonPrivacy_)
    , deltaPrivacy_(other.deltaPrivacy_) {
    other.handle_ = nullptr;
}

CSSecureFederatedLearning& CSSecureFederatedLearning::operator=(CSSecureFederatedLearning&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            cswift_secure_aggregator_destroy(handle_);
        }
        handle_ = other.handle_;
        privacyLevel_ = other.privacyLevel_;
        encryptionMode_ = other.encryptionMode_;
        noiseType_ = other.noiseType_;
        epsilonPrivacy_ = other.epsilonPrivacy_;
        deltaPrivacy_ = other.deltaPrivacy_;
        other.handle_ = nullptr;
    }
    return *this;
}

/**
 * @brief Add differential privacy noise to gradients
 * 
 * Applies mathematically rigorous differential privacy noise to protect
 * individual client contributions.
 * 
 * @param gradients Gradient vector to protect
 * @param sensitivity Sensitivity of the gradient computation
 * @return CS_SUCCESS or error code
 */
CSError CSSecureFederatedLearning::addDifferentialPrivacy(std::vector<float>& gradients, float sensitivity) {
    return static_cast<CSError>(cswift_add_dp_noise(
        handle_,
        gradients.data(),
        static_cast<int32_t>(gradients.size()),
        sensitivity
    ));
}

/**
 * @brief Encrypt gradients for secure transmission
 * 
 * Uses state-of-the-art encryption (AES-256-GCM or ChaCha20-Poly1305)
 * to protect gradients during transmission.
 * 
 * @param gradients Gradients to encrypt
 * @return Encrypted data
 */
std::vector<uint8_t> CSSecureFederatedLearning::encryptGradients(const std::vector<float>& gradients) {
    void* encryptedData = nullptr;
    int encryptedSize = 0;
    
    CSError result = static_cast<CSError>(cswift_encrypt_gradients(
        handle_,
        gradients.data(),
        static_cast<int32_t>(gradients.size()),
        &encryptedData,
        &encryptedSize
    ));
    
    if (result != CS_SUCCESS || !encryptedData) {
        throw CSException(result, "Failed to encrypt gradients");
    }
    
    std::vector<uint8_t> encrypted(encryptedSize);
    std::memcpy(encrypted.data(), encryptedData, encryptedSize);  // ZERO_COPY_ALLOWED - secure data handling
    free(encryptedData);
    
    return encrypted;
}

/**
 * @brief Decrypt gradients after transmission
 * 
 * @param encryptedData Encrypted gradient data
 * @param maxGradients Maximum number of gradients to decrypt
 * @return Decrypted gradients
 */
std::vector<float> CSSecureFederatedLearning::decryptGradients(const std::vector<uint8_t>& encryptedData, size_t maxGradients) {
    std::vector<float> gradients(maxGradients);
    
    int32_t actualCount = cswift_decrypt_gradients(
        handle_,
        encryptedData.data(),
        static_cast<int32_t>(encryptedData.size()),
        gradients.data(),
        static_cast<int32_t>(maxGradients)
    );
    
    if (actualCount < 0) {
        throw CSException(static_cast<CSError>(actualCount), "Failed to decrypt gradients");
    }
    
    gradients.resize(actualCount);
    return gradients;
}

/**
 * @brief Securely aggregate gradients from multiple clients
 * 
 * Performs privacy-preserving aggregation with cryptographic security
 * and differential privacy protection.
 * 
 * @param clientGradients Vector of gradient vectors from all clients
 * @return Securely aggregated gradients
 */
std::vector<float> CSSecureFederatedLearning::secureAggregate(const std::vector<std::vector<float>>& clientGradients) {
    if (clientGradients.empty()) {
        return {};
    }
    
    // Prepare client gradient pointers
    std::vector<const float*> gradientPointers;
    std::vector<int32_t> gradientCounts;
    
    for (const auto& gradients : clientGradients) {
        gradientPointers.push_back(gradients.data());
        gradientCounts.push_back(static_cast<int32_t>(gradients.size()));
    }
    
    // Allocate output buffer
    size_t maxOutputSize = clientGradients[0].size();
    std::vector<float> aggregated(maxOutputSize);
    
    int32_t actualCount = cswift_secure_aggregate(
        handle_,
        gradientPointers.data(),
        gradientCounts.data(),
        static_cast<int32_t>(clientGradients.size()),
        aggregated.data(),
        static_cast<int32_t>(maxOutputSize)
    );
    
    if (actualCount < 0) {
        throw CSException(static_cast<CSError>(actualCount), "Failed to aggregate gradients");
    }
    
    aggregated.resize(actualCount);
    return aggregated;
}

/**
 * @brief Privacy-preserving gradient compression
 * 
 * Compresses gradients while maintaining privacy through noise injection
 * and sparse representation.
 * 
 * @param gradients Gradients to compress
 * @param compressionRatio Compression ratio (0.01 = 1% of gradients kept)
 * @param noiseScale Scale of privacy noise to add
 * @return Compressed gradients as (indices, values) pair
 */
std::pair<std::vector<int32_t>, std::vector<float>> 
CSSecureFederatedLearning::compressWithPrivacy(const std::vector<float>& gradients, float compressionRatio, float noiseScale) {
    
    size_t maxOutputSize = static_cast<size_t>(gradients.size() * compressionRatio) + 1;
    std::vector<int32_t> indices(maxOutputSize);
    std::vector<float> values(maxOutputSize);
    
    int32_t actualCount = cswift_compress_gradients_private(
        gradients.data(),
        static_cast<int32_t>(gradients.size()),
        compressionRatio,
        noiseScale,
        indices.data(),
        values.data(),
        static_cast<int32_t>(maxOutputSize)
    );
    
    if (actualCount < 0) {
        throw CSException(static_cast<CSError>(actualCount), "Failed to compress gradients");
    }
    
    indices.resize(actualCount);
    values.resize(actualCount);
    
    return std::make_pair(std::move(indices), std::move(values));
}

// Getters
CSPrivacyLevel CSSecureFederatedLearning::getPrivacyLevel() const { return privacyLevel_; }
CSEncryptionMode CSSecureFederatedLearning::getEncryptionMode() const { return encryptionMode_; }
CSDifferentialPrivacyNoise CSSecureFederatedLearning::getNoiseType() const { return noiseType_; }
float CSSecureFederatedLearning::getEpsilonPrivacy() const { return epsilonPrivacy_; }
float CSSecureFederatedLearning::getDeltaPrivacy() const { return deltaPrivacy_; }

} // namespace cswift
