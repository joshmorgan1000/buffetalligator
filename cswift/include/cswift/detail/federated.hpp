#ifndef CSWIFT_FEDERATED_HPP
#define CSWIFT_FEDERATED_HPP

#include <cswift/detail/globals.hpp>
#include <cswift/detail/network.hpp>

namespace cswift {

// ============================================================================
// Secure Federated Learning Interface
// ============================================================================

/**
 * @brief Privacy protection levels for federated learning
 */
enum class CSPrivacyLevel {
    None = 0,
    Basic = 1,      // Basic differential privacy
    Standard = 2,   // Standard DP + secure aggregation
    Maximum = 3     // Maximum privacy with advanced techniques
};

/**
 * @brief Encryption modes for gradient protection
 */
enum class CSEncryptionMode {
    None = 0,
    AES256 = 1,         // AES-256-GCM encryption
    ChaCha20 = 2,       // ChaCha20-Poly1305 encryption
    Homomorphic = 3     // Homomorphic encryption (experimental)
};

/**
 * @brief Differential privacy noise types
 */
enum class CSDifferentialPrivacyNoise {
    Gaussian = 0,       // Gaussian noise
    Laplacian = 1,      // Laplacian noise
    Exponential = 2     // Exponential mechanism
};

/**
 * @brief Enterprise-grade secure federated learning aggregator
 * 
 * Provides the same level of privacy and security used by Apple, Google, and Meta
 * for production federated learning systems. Includes:
 * - Differential privacy with configurable epsilon/delta
 * - Secure aggregation with cryptographic protection
 * - Client authentication and key management
 * - Privacy-preserving gradient compression
 */
class CSSecureFederatedLearning {
private:
    CSHandle handle_;
    CSPrivacyLevel privacyLevel_;
    CSEncryptionMode encryptionMode_;
    CSDifferentialPrivacyNoise noiseType_;
    float epsilonPrivacy_;
    float deltaPrivacy_;
    
public:
    /**
     * @brief Create secure federated learning aggregator
     * 
     * @param privacyLevel Privacy protection level
     * @param encryptionMode Encryption mode for gradient protection
     * @param noiseType Differential privacy noise type
     * @param epsilonPrivacy Privacy budget epsilon (smaller = more private)
     * @param deltaPrivacy Privacy budget delta (smaller = more private)
     */
    CSSecureFederatedLearning(CSPrivacyLevel privacyLevel = CSPrivacyLevel::Standard,
                             CSEncryptionMode encryptionMode = CSEncryptionMode::AES256,
                             CSDifferentialPrivacyNoise noiseType = CSDifferentialPrivacyNoise::Gaussian,
                             float epsilonPrivacy = 1.0f,
                             float deltaPrivacy = 1e-5f);
    
    ~CSSecureFederatedLearning();
    
    // Move constructor and assignment
    CSSecureFederatedLearning(CSSecureFederatedLearning&& other) noexcept;
    
    CSSecureFederatedLearning& operator=(CSSecureFederatedLearning&& other) noexcept;
    
    // Delete copy constructor and assignment
    CSSecureFederatedLearning(const CSSecureFederatedLearning&) = delete;
    CSSecureFederatedLearning& operator=(const CSSecureFederatedLearning&) = delete;
    
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
    CSError addDifferentialPrivacy(std::vector<float>& gradients, float sensitivity = 1.0f);
    
    /**
     * @brief Encrypt gradients for secure transmission
     * 
     * Uses state-of-the-art encryption (AES-256-GCM or ChaCha20-Poly1305)
     * to protect gradients during transmission.
     * 
     * @param gradients Gradients to encrypt
     * @return Encrypted data
     */
    std::vector<uint8_t> encryptGradients(const std::vector<float>& gradients);
    
    /**
     * @brief Decrypt gradients after transmission
     * 
     * @param encryptedData Encrypted gradient data
     * @param maxGradients Maximum number of gradients to decrypt
     * @return Decrypted gradients
     */
    std::vector<float> decryptGradients(const std::vector<uint8_t>& encryptedData,
                                       size_t maxGradients = 1000000);
    
    /**
     * @brief Securely aggregate gradients from multiple clients
     * 
     * Performs privacy-preserving aggregation with cryptographic security
     * and differential privacy protection.
     * 
     * @param clientGradients Vector of gradient vectors from all clients
     * @return Securely aggregated gradients
     */
    std::vector<float> secureAggregate(const std::vector<std::vector<float>>& clientGradients);
    
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
    compressWithPrivacy(const std::vector<float>& gradients,
                       float compressionRatio = 0.01f,
                       float noiseScale = 0.1f);
    
    // Getters
    CSPrivacyLevel getPrivacyLevel() const;
    CSEncryptionMode getEncryptionMode() const;
    CSDifferentialPrivacyNoise getNoiseType() const;
    float getEpsilonPrivacy() const;
    float getDeltaPrivacy() const;
};

} // namespace cswift

#endif // CSWIFT_FEDERATED_HPP
