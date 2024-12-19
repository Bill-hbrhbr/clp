#ifndef CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP
#define CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP

#include <cstddef>
#include <cstdint>

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

    /**
     * Initializes the compression stream with the given compression level
     *
     * @param file_writer
     * @param compression_level
     */
    auto open(FileWriter& file_writer, int compression_level) -> void;

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

private:
    class LzmaStreamWrapper {
    public:
        // Constructor
        LzmaStreamWrapper() = default;

        // Destructor
        ~LzmaStreamWrapper() = default;

        // Delete copy constructor and assignment operator
        LzmaStreamWrapper(LzmaStreamWrapper const&) = delete;
        auto operator=(LzmaStreamWrapper const&) -> LzmaStreamWrapper& = delete;

        // Default move constructor and assignment operator
        LzmaStreamWrapper(LzmaStreamWrapper&&) noexcept = default;
        auto operator=(LzmaStreamWrapper&&) noexcept -> LzmaStreamWrapper& = default;

        [[nodiscard]] static auto is_flush_action(lzma_action action) -> bool {
            return LZMA_SYNC_FLUSH == action || LZMA_FULL_FLUSH == action
                   || LZMA_FULL_BARRIER == action || LZMA_FINISH == action;
        }

        /**
         * Initializes an LZMA compression encoder and its streams
         *
         * @param compression_level
         * @param dict_size Dictionary size that specifies how many bytes of the
         *                  recently processed uncompressed data to keep in the memory
         * @param check Type of integrity check calculated from the uncompressed data.
         * LZMA_CHECK_CRC64 is the default in the xz command line tool. If the .xz file needs to be
         * decompressed with XZ-Embedded, use LZMA_CHECK_CRC32 instead.
         */
        auto init_lzma_encoder(
                int compression_level,
                size_t dict_size,
                lzma_check check = LZMA_CHECK_CRC64
        ) -> void;

        /**
         * Tears down the lzma stream after flushing any remaining buffered output
         */
        auto close_lzma() -> void;

        /**
         * Invokes lzma_code() repeatedly with LZMA_RUN until the input is exhausted
         *
         * At the end of the workflow, the last bytes of encoded data may still be buffered in the
         * LZMA stream and thus not immediately available at the output block buffer.
         *
         * Assumes input stream and output block buffer are both in valid states.
         * @throw `OperationFailed` if LZMA returns an unexpected error value
         */
        auto encode_lzma() -> void;

        /**
         * Invokes lzma_code() repeatedly with the given flushing action until all encoded data is
         * made available at the output block buffer
         *
         * Once flushing starts, the workflow action needs to stay the same until flushing is
         * signaled complete by LZMA (aka LZMA_STREAM_END is reached). See also:
         * https://github.com/tukaani-project/xz/blob/master/src/liblzma/api/lzma/base.h#L274
         *
         * Assumes input stream and output block buffer are both in valid states.
         * @param flush_action
         * @throw `OperationFailed` if the provided action is not an LZMA flush
         *        action, or if LZMA returns an unexpected error value
         */
        auto flush_lzma(lzma_action flush_action) -> void;

        /**
         * Flushes the current compressed data in the output block buffer to the output file
         * handler. Reset the output block buffer to receive new data.
         *
         * @param force Whether to flush even if the output buffer is not full
         */
        auto flush_stream_output_block_buffer(bool force) -> void;

        auto attach_input_src(uint8_t const* data_ptr, size_t data_length) -> void {
            m_compression_stream.next_in = data_ptr;
            m_compression_stream.avail_in = data_length;
        }

        // Repeated calls to this function resets the output buffer to its initial state.
        auto attach_output_buffer() -> void {
            m_compression_stream.next_out = m_compressed_stream_block_buffer.data();
            m_compression_stream.avail_out = cCompressedStreamBlockBufferSize;
        }

        auto detach_input_src() -> void {
            m_compression_stream.next_in = nullptr;
            m_compression_stream.avail_in = 0;
        }

        auto detach_output_buffer() -> void {
            m_compression_stream.next_out = nullptr;
            m_compression_stream.avail_out = 0;
        }

        auto set_compressed_stream_file_writer(FileWriter* writer) -> void {
            m_compressed_stream_file_writer = writer;
        }

        [[nodiscard]] auto missing_compressed_stream_file_writer() const -> bool {
            return nullptr == m_compressed_stream_file_writer;
        }

    private:
        static constexpr size_t cCompressedStreamBlockBufferSize{4096};  // 4KiB

        // Compressed stream variables
        lzma_stream m_compression_stream = LZMA_STREAM_INIT;
        Array<uint8_t> m_compressed_stream_block_buffer{cCompressedStreamBlockBufferSize};
        FileWriter* m_compressed_stream_file_writer{nullptr};
    };

    // Variables
    LzmaStreamWrapper m_stream_handler;
    size_t m_dict_size{cDefaultDictionarySize};
    size_t m_uncompressed_stream_pos{0};
};
}  // namespace clp::streaming_compression::lzma

#endif  // CLP_STREAMING_COMPRESSION_LZMA_COMPRESSOR_HPP
