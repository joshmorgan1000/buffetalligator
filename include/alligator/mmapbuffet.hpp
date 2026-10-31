#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyMmap
 * @file mmapbuffet.hpp
 * @brief The mmap scratch-file buffer: zero-filled MAP_SHARED mappings of unlinked files in a caller-chosen
 * directory, for disk- or RAM-filesystem-backed Slices.
 */
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- MmapBuffet
 * @class MmapBuffet
 * @brief One scratch-file mapping in the chosen directory; a Slice is claimed over the whole of it.
 */
class MmapBuffet : public IBuffet {
private:
    int descriptor_ = -1;  ///< The scratch file's descriptor, open for the mapping's lifetime.
    /** ------------------------------------------------------------------------------------------- release
     * @brief Unmaps and closes the scratch file; defined in mmap_allocator.cpp.
     */
    void release();
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Records the size; allocate() creates and maps the scratch file.
     * @param size Buffer size in bytes.
     */
    explicit MmapBuffet(size_t size = 0);
    MmapBuffet(const MmapBuffet&) = delete;
    MmapBuffet& operator=(const MmapBuffet&) = delete;
    MmapBuffet(MmapBuffet&&) = delete;
    MmapBuffet& operator=(MmapBuffet&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases the mapping by unmapping and closing the scratch file.
     */
    ~MmapBuffet() override { release(); }
    void allocate() override;
    void free() override;
    size_t size() const override;
    void set_size(size_t size) override;
    void* host_ptr() override;
    const void* host_ptr() const override;
    /** ------------------------------------------------------------------------------------------- Set Directory
     * @brief Checks the scratch directory once; every mapping after it lands there.
     * @param directory A directory on the filesystem that should back the scratch files.
     */
    static void set_directory(const std::string& directory);
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief A Slice over one whole new scratch-file mapping.
     * @param size Bytes.
     * @return The Slice; the mapping goes with its last handle.
     */
    static Slice claim(size_t size) {
        std::shared_ptr<IBuffet> buffer = std::make_shared<MmapBuffet>(size);
        buffer->allocate();
        return IBuffet::embue_id(buffer);
    }
    /** ------------------------------------------------------------------------------------------- Flush
     * @brief Synchronously writes a Slice's page-aligned mapped range back to its scratch file.
     * @param slice A Slice claimed from this type.
     */
    static void flush(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Descriptor
     * @brief The borrowed descriptor of the scratch file behind a Slice.
     * @param slice A Slice claimed from this type.
     * @return The descriptor, valid while the Slice lives.
     */
    static int file_descriptor(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Offset
     * @brief A Slice's byte offset within its scratch file, including sub-slice offsets.
     * @param slice A Slice claimed from this type.
     * @return The byte offset.
     */
    static uint64_t file_offset(const Slice& slice);
};
} // namespace buffetalligator
