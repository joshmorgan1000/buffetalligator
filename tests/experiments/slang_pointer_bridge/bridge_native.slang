/** --------------------------------------------------------------------------------------------------------- Native Pointer Module
 * @brief Exports a native address operation for a GLSL translation unit.
 */
public uint64_t bridge_region(uint64_t directory, uint id) {
    return ((uint64_t*)directory)[(id >> 3u) & 63u];
}
