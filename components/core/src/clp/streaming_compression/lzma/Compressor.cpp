#include "Compressor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <lzma.h>
#include <spdlog/spdlog.h>

#include "../../ErrorCode.hpp"
#include "../../FileWriter.hpp"
#include "../../TraceableException.hpp"
#include "../../type_utils.hpp"
#include "Constants.hpp"

namespace clp::streaming_compression::lzma {
auto Compressor::open(FileWriter& file_writer, int compression_level) -> void {
    if (false == m_stream_handler.missing_compressed_stream_file_writer()) {
        throw OperationFailed(ErrorCode_NotReady, __FILENAME__, __LINE__);
    }

    if (compression_level < cMinCompressionLevel || compression_level > cMaxCompressionLevel) {
        throw OperationFailed(ErrorCode_Unsupported, __FILENAME__, __LINE__);
    }

    m_stream_handler.init_lzma_encoder(compression_level, m_dict_size);
    m_stream_handler.detach_input_src();
    m_stream_handler.attach_output_buffer();
    m_stream_handler.set_compressed_stream_file_writer(&file_writer);
    m_uncompressed_stream_pos = 0;
}

auto Compressor::close() -> void {
    if (m_stream_handler.missing_compressed_stream_file_writer()) {
        throw OperationFailed(ErrorCode_NotInit, __FILENAME__, __LINE__);
    }
    m_stream_handler.close_lzma();
    m_stream_handler.set_compressed_stream_file_writer(nullptr);
}

auto Compressor::write(char const* data, size_t data_length) -> void {
    if (m_stream_handler.missing_compressed_stream_file_writer()) {
        throw OperationFailed(ErrorCode_NotInit, __FILENAME__, __LINE__);
    }

    if (0 == data_length) {
        return;
    }

    if (nullptr == data) {
        throw OperationFailed(ErrorCode_BadParam, __FILENAME__, __LINE__);
    }

    m_stream_handler.attach_input_src(
            clp::size_checked_pointer_cast<uint8_t const>(data),
            data_length
    );
    m_stream_handler.encode_lzma();
    m_stream_handler.detach_input_src();
    m_uncompressed_stream_pos += data_length;
}

auto Compressor::flush() -> void {
    if (m_stream_handler.missing_compressed_stream_file_writer()) {
        throw OperationFailed(ErrorCode_NotInit, __FILENAME__, __LINE__);
    }
    m_stream_handler.flush_lzma(LZMA_SYNC_FLUSH);
}

auto Compressor::try_get_pos(size_t& pos) const -> ErrorCode {
    if (m_stream_handler.missing_compressed_stream_file_writer()) {
        return ErrorCode_NotInit;
    }

    pos = m_uncompressed_stream_pos;
    return ErrorCode_Success;
}

auto Compressor::LzmaStreamWrapper::init_lzma_encoder(
        int compression_level,
        size_t dict_size,
        lzma_check check
) -> void {
    m_compression_stream = LZMA_STREAM_INIT;
    lzma_options_lzma options;
    if (0 != lzma_lzma_preset(&options, compression_level)) {
        SPDLOG_ERROR("Failed to initialize LZMA options' compression level.");
        throw OperationFailed(ErrorCode_BadParam, __FILENAME__, __LINE__);
    }
    options.dict_size = dict_size;
    std::array<lzma_filter, 2> filters{{
            {.id = LZMA_FILTER_LZMA2, .options = &options},
            {.id = LZMA_VLI_UNKNOWN, .options = nullptr},
    }};

    auto const rc = lzma_stream_encoder(&m_compression_stream, filters.data(), check);
    if (LZMA_OK == rc) {
        return;
    }

    char const* msg{nullptr};
    switch (rc) {
        case LZMA_MEM_ERROR:
            msg = "Memory allocation failed";
            break;

        case LZMA_OPTIONS_ERROR:
            msg = "Specified preset is not supported";
            break;

        case LZMA_UNSUPPORTED_CHECK:
            msg = "Specified integrity check is not supported";
            break;

        case LZMA_PROG_ERROR:
            msg = "Input arguments are not sane";
            break;

        default:
            msg = "Unknown error";
            break;
    }

    SPDLOG_ERROR("Error initializing the encoder: {} (error code {})", msg, static_cast<int>(rc));
    throw OperationFailed(ErrorCode_BadParam, __FILENAME__, __LINE__);
}

auto Compressor::LzmaStreamWrapper::close_lzma() -> void {
    if (m_compression_stream.avail_in > 0) {
        SPDLOG_ERROR("Tried to close LZMA compressor with unprocessed input data.");
        throw OperationFailed(ErrorCode_Failure, __FILENAME__, __LINE__);
    }
    flush_lzma(LZMA_FINISH);
    // Deallocates LZMA stream's internal data structures
    lzma_end(&m_compression_stream);
    detach_output_buffer();
}

auto Compressor::LzmaStreamWrapper::encode_lzma() -> void {
    while (m_compression_stream.avail_in > 0) {
        flush_stream_output_block_buffer(false /*force*/);

        auto const rc = lzma_code(&m_compression_stream, LZMA_RUN);
        switch (rc) {
            case LZMA_OK:
                break;
            case LZMA_BUF_ERROR:
                SPDLOG_ERROR(
                        "LZMA compressor input stream is corrupt. No encoding progress can be made."
                );
                throw OperationFailed(ErrorCode_Failure, __FILENAME__, __LINE__);
            default:
                SPDLOG_ERROR(
                        "lzma_code() returned an unexpected value - {}.",
                        static_cast<int>(rc)
                );
                throw OperationFailed(ErrorCode_Failure, __FILENAME__, __LINE__);
        }
    }
}

auto Compressor::LzmaStreamWrapper::flush_lzma(lzma_action flush_action) -> void {
    if (false == is_flush_action(flush_action)) {
        SPDLOG_ERROR(
                "lzma_code() supplied with invalid flush action - {}.",
                static_cast<int>(flush_action)
        );
        throw OperationFailed(ErrorCode_BadParam, __FILENAME__, __LINE__);
    }

    bool flushed{false};
    while (false == flushed) {
        flush_stream_output_block_buffer(false /*force*/);
        auto const rc = lzma_code(&m_compression_stream, flush_action);
        switch (rc) {
            case LZMA_OK:
                break;
            case LZMA_STREAM_END:
                // NOTE: flush may not have completed if a multithreaded encoder is using action
                // LZMA_FULL_BARRIER. For now, we skip this check.
                flushed = true;
                break;
            case LZMA_BUF_ERROR:
                // NOTE: this can happen if we are using LZMA_FULL_FLUSH or LZMA_FULL_BARRIER. These
                // two actions keeps encoding input data alongside flushing buffered encoded data.
                SPDLOG_ERROR(
                        "LZMA compressor input stream is corrupt. No encoding progress can be made."
                );
                throw OperationFailed(ErrorCode_Failure, __FILENAME__, __LINE__);
            default:
                SPDLOG_ERROR(
                        "lzma_code() returned an unexpected value - {}.",
                        static_cast<int>(rc)
                );
                throw OperationFailed(ErrorCode_Failure, __FILENAME__, __LINE__);
        }
    }
    flush_stream_output_block_buffer(true /*force*/);
}

auto Compressor::LzmaStreamWrapper::flush_stream_output_block_buffer(bool force) -> void {
    if (m_compression_stream.avail_out > 0 && false == force) {
        return;
    }
    if (cCompressedStreamBlockBufferSize == m_compression_stream.avail_out) {
        return;
    }
    m_compressed_stream_file_writer->write(
            clp::size_checked_pointer_cast<char>(m_compressed_stream_block_buffer.data()),
            cCompressedStreamBlockBufferSize - m_compression_stream.avail_out
    );
    attach_output_buffer();
}

}  // namespace clp::streaming_compression::lzma
