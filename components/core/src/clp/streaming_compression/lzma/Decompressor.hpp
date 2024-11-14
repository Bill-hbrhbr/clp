#ifndef CLP_STREAMING_COMPRESSION_LZMA_DECOMPRESSOR_HPP
#define CLP_STREAMING_COMPRESSION_LZMA_DECOMPRESSOR_HPP

// C++ standard libraries
#include <memory>
#include <string>

// Compression libraries
#include <lzma.h>
#include <zlib.h>

// Boost libraries
#include <boost/iostreams/device/mapped_file.hpp>

// Project headers
#include "../../FileReader.hpp"
#include "../../TraceableException.hpp"
#include "../Decompressor.hpp"

namespace clp::streaming_compression::lzma {
class Decompressor : public ::clp::streaming_compression::Decompressor {
public:
    // Types
    class OperationFailed : public TraceableException {
    public:
        // Constructors
        OperationFailed(ErrorCode error_code, char const* const filename, int line_number)
                : TraceableException(error_code, filename, line_number) {}

        // Methods
        char const* what() const noexcept override {
            return "streaming_compression::lzma::Decompressor operation failed";
        }
    };

    // Constructor
    Decompressor();

    // Destructor
    ~Decompressor();

    // Explicitly disable copy and move constructor/assignment
    Decompressor(Decompressor const&) = delete;
    Decompressor& operator=(Decompressor const&) = delete;

    // Methods implementing the ReaderInterface
    /**
     * Tries to read up to a given number of bytes from the decompressor
     * @param buf
     * @param num_bytes_to_read The number of bytes to try and read
     * @param num_bytes_read The actual number of bytes read
     * @return Same as FileReader::try_read if the decompressor is attached to a file
     * @return ErrorCode_NotInit if the decompressor is not open
     * @return ErrorCode_BadParam if buf is invalid
     * @return ErrorCode_EndOfFile on EOF
     * @return ErrorCode_Failure on decompression failure
     * @return ErrorCode_Success on success
     */
    ErrorCode try_read(char* buf, size_t num_bytes_to_read, size_t& num_bytes_read) override;

    /**
     */
    void exact_read(char* buf, size_t num_bytes_to_read, size_t& num_bytes_read);

    /**
     * Tries to seek from the beginning to the given position
     * @param pos
     * @return ErrorCode_NotInit if the decompressor is not open
     * @return Same as ReaderInterface::try_read_exact_length
     * @return ErrorCode_Success on success
     */
    ErrorCode try_seek_from_begin(size_t pos) override;
    /**
     * Tries to get the current position of the read head
     * @param pos Position of the read head in the file
     * @return ErrorCode_NotInit if the decompressor is not open
     * @return ErrorCode_Success on success
     */
    ErrorCode try_get_pos(size_t& pos) override;

    // Methods implementing the Decompressor interface
    void close() override;
    /**
     * Decompresses and copies the range of uncompressed data described by
     * decompressed_stream_pos and extraction_len into extraction_buf
     * @param decompressed_stream_pos
     * @param extraction_buf
     * @param extraction_len
     * @return Same as streaming_compression::zstd::Decompressor::try_seek_from_begin
     * @return Same as ReaderInterface::try_read_exact_length
     */
    ErrorCode get_decompressed_stream_region(
            size_t decompressed_stream_pos,
            char* extraction_buf,
            size_t extraction_len
    ) override;

    // Methods
    /***
     * Initialize streaming decompressor to decompress from the specified compressed data buffer
     * @param compressed_data_buf
     * @param compressed_data_buf_size
     */
    void open(char const* compressed_data_buf, size_t compressed_data_buf_size) override;

    /***
     * Initialize streaming decompressor to decompress from a compressed file specified by the
     * given path
     * @param compressed_file_path
     * @param decompressed_stream_block_size
     * @return ErrorCode_Failure if the provided path cannot be memory mapped
     * @return ErrorCode_Success on success
     */
    ErrorCode open(std::string const& compressed_file_path);

    /**
     * Initializes the decompressor to decompress from an open file
     * @param file_reader
     * @param file_read_buffer_capacity The maximum amount of data to read from a file at a time
     */
    void open(FileReader& file_reader, size_t file_read_buffer_capacity) override;

private:
    // Enum class
    enum class InputType {
        NotInitialized,  // Note: do nothing but generate an error to prevent this required
                         // parameter is not initialized properly
        CompressedDataBuf,
        MemoryMappedCompressedFile,
        File
    };

    // Methods
    /**
     * Reset streaming decompression state so it will start decompressing from the beginning of
     * the stream afterwards
     */
    void reset_stream();

    void init_decoder(lzma_stream* strm);

    // Variables
    InputType m_input_type;

    // Compressed stream variables
    lzma_stream* m_decompression_stream{nullptr};

    boost::iostreams::mapped_file_source m_memory_mapped_compressed_file;
    FileReader* m_file_reader;
    size_t m_file_reader_initial_pos;
    std::unique_ptr<char[]> m_file_read_buffer;
    size_t m_file_read_buffer_length;
    size_t m_file_read_buffer_capacity;

    size_t m_decompressed_stream_pos;
    size_t m_unused_decompressed_stream_block_size;
    std::unique_ptr<char[]> m_unused_decompressed_stream_block_buffer;

    char const* m_compressed_stream_block;
    size_t m_compressed_stream_block_size;
};
}  // namespace clp::streaming_compression::lzma
#endif  // CLP_STREAMING_COMPRESSION_LZMA_DECOMPRESSOR_HPP
