#ifndef CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP
#define CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP

#include <cstddef>
#include <memory>

#include <lzma.h>

#include "../../Array.hpp"
#include "../../ErrorCode.hpp"
#include "../../FileWriter.hpp"
#include "../../TraceableException.hpp"
#include "../Compressor.hpp"
#include "Constants.hpp"

namespace clp::streaming_compression::lzma {
class Compressor : public ::clp::streaming_compression::Compressor {
public:
    // Types
    class OperationFailed : public TraceableException {
    public:
        // Constructors
        OperationFailed(ErrorCode error_code, char const* const filename, int line_number)
                : TraceableException(error_code, filename, line_number) {}

        // Methods
        [[nodiscard]] auto what() const noexcept -> char const* override {
            return "streaming_compression::lzma::Compressor operation failed";
        }
    };

    // Constructor
    Compressor() = default;

    // Destructor
    ~Compressor() override = default;

    // Delete copy constructor and assignment operator
    Compressor(Compressor const&) = delete;
    auto operator=(Compressor const&) -> Compressor& = delete;

    // Default move constructor and assignment operator
    Compressor(Compressor&&) noexcept = default;
    auto operator=(Compressor&&) noexcept -> Compressor& = default;

    // Methods implementing the WriterInterface
    /**
     * Writes the given data to the compressor
     * @param data
     * @param data_length
     */
    auto write(char const* data, size_t data_length) -> void override;

    /**
     * Writes any internally buffered data to file and ends the current frame
     *
     * Forces all the encoded data buffered by LZMA to be available at output
     */
    auto flush() -> void override;

    /**
     * Tries to get the current position of the write head
     * @param pos Position of the write head
     * @return ErrorCode_NotInit if the compressor is not open
     * @return ErrorCode_Success on success
     */
    auto try_get_pos(size_t& pos) const -> ErrorCode override;

    /**
     * Closes the compressor
     */
    auto close() -> void override;

    // Methods implementing the Compressor interface
    /**
     * Initializes the compression stream with the default compression level
     * @param file_writer
     */
    auto open(FileWriter& file_writer) -> void override {
        this->open(file_writer, cDefaultCompressionLevel);
    }

    /**
     * Initializes the compression stream with the given compression level
     * @param file_writer
     * @param compression_level
     */
    auto open(FileWriter& file_writer, int compression_level) -> void;

private:
    static constexpr size_t cCompressedStreamBlockBufferSize{4096};  // 4KiB

    /**
     * Invoke lzma_code() encoding workflow once with LZMA_RUN
     *
     * The encoded data may be buffered and thus not immediately available at
     * the output block.
     */
    auto encode_lzma_once() -> void;

    /**
     * Invoke lzma_code() repeatedly with the given flushing action until all
     * encoded data is made available at the output block
     *
     * Once flushing starts, the workflow action needs to stay the same until
     * flushing is signaled completed by LZMA (aka LZMA_STREAM_END is reached).
     * See also: https://github.com/tukaani-project/xz/blob/master/src/liblzma/api/lzma/base.h#L274
     *
     * @param flush_action
     */
    auto flush_lzma(lzma_action flush_action) -> void;

    /**
     * Flushes the current compressed data in the lzma output buffer to the
     * output file handler. Reset the compression buffer to receive new data.
     */
    auto flush_stream_output_block_buffer() -> void;

    // Variables
    FileWriter* m_compressed_stream_file_writer{nullptr};

    // Compressed stream variables
    lzma_stream m_compression_stream;
    size_t m_dict_size{cDefaultDictionarySize};

    Array<uint8_t> m_compressed_stream_block_buffer{cCompressedStreamBlockBufferSize};

    size_t m_uncompressed_stream_pos{0};
};
}  // namespace clp::streaming_compression::lzma

#endif  // CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP
