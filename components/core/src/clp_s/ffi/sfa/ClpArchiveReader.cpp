#include "ClpArchiveReader.hpp"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <ystdlib/error_handling/Result.hpp>

#include <clp/BufferReader.hpp>
#include <clp_s/archive_constants.hpp>
#include <clp_s/ArchiveReader.hpp>
#include <clp_s/ffi/sfa/LogEvent.hpp>
#include <clp_s/ffi/sfa/SfaErrorCode.hpp>
#include <clp_s/InputConfig.hpp>

namespace clp_s::ffi::sfa {
template <typename ReturnType>
using Result = ystdlib::error_handling::Result<ReturnType>;

auto ClpArchiveReader::create(std::string_view archive_path) -> Result<ClpArchiveReader> {
    std::unique_ptr<clp_s::ArchiveReader> reader;

    try {
        auto path{get_path_object_for_raw_path(archive_path)};
        reader = std::make_unique<clp_s::ArchiveReader>();
        reader->open(path, NetworkAuthOption{});
        auto clp_archive_reader{ClpArchiveReader{std::move(reader), nullptr}};
        YSTDLIB_ERROR_HANDLING_TRYV(clp_archive_reader.precompute_archive_metadata());
        return clp_archive_reader;
    } catch (std::bad_alloc const&) {
        SPDLOG_ERROR(
                "Failed to create ClpArchiveReader for archive {}: out of memory.",
                archive_path
        );
        return SfaErrorCode{SfaErrorCodeEnum::NoMemory};
    } catch (std::exception const& ex) {
        SPDLOG_ERROR("Exception while creating ClpArchiveReader: {}", ex.what());
        return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
    }
}

auto ClpArchiveReader::create(std::vector<char>&& archive_data) -> Result<ClpArchiveReader> {
    // `clp_s::ArchiveReader` requires an archive ID, but `clp_s::ffi::sfa::ClpArchiveReader` never
    // uses it. Provide a dummy value solely to satisfy the constructor.
    constexpr std::string_view cDefaultArchiveId{"default"};

    std::unique_ptr<clp_s::ArchiveReader> archive_reader;
    std::shared_ptr<std::vector<char>> archive_data_owner;

    try {
        archive_data_owner = std::make_shared<std::vector<char>>(std::move(archive_data));
        auto reader{std::make_shared<clp::BufferReader>(
                archive_data_owner->data(),
                archive_data_owner->size()
        )};

        archive_reader = std::make_unique<clp_s::ArchiveReader>();
        archive_reader->open(reader, cDefaultArchiveId);
        auto clp_archive_reader{
                ClpArchiveReader{std::move(archive_reader), std::move(archive_data_owner)}
        };
        YSTDLIB_ERROR_HANDLING_TRYV(clp_archive_reader.precompute_archive_metadata());
        return clp_archive_reader;
    } catch (std::bad_alloc const&) {
        SPDLOG_ERROR("Failed to create ClpArchiveReader: out of memory.");
        return SfaErrorCode{SfaErrorCodeEnum::NoMemory};
    } catch (std::exception const& ex) {
        SPDLOG_ERROR("Exception while creating ClpArchiveReader: {}", ex.what());
        return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
    }
}

ClpArchiveReader::ClpArchiveReader(
        std::unique_ptr<clp_s::ArchiveReader> reader,
        std::shared_ptr<std::vector<char>> archive_data
)
        : m_archive_reader{std::move(reader)},
          m_archive_data{std::move(archive_data)},
          m_decoded_text_buffers(cChunkBufferSize * cNumChunkBuffers) {}

ClpArchiveReader::ClpArchiveReader(ClpArchiveReader&& rhs) noexcept
        : m_decoded_text_buffers(cChunkBufferSize * cNumChunkBuffers) {
    move_from(rhs);
}

auto ClpArchiveReader::operator=(ClpArchiveReader&& rhs) noexcept -> ClpArchiveReader& {
    if (this == &rhs) {
        return *this;
    }

    close();
    move_from(rhs);
    return *this;
}

ClpArchiveReader::~ClpArchiveReader() noexcept {
    close();
}

auto ClpArchiveReader::close() noexcept -> void {
    // FFI frontends may invoke destruction paths multiple times (e.g., explicit close followed by
    // GC finalization). Guard against this by checking for a null reader before attempting to
    // close.
    if (nullptr != m_archive_reader) {
        try {
            m_archive_reader->close();
        } catch (std::exception const& ex) {
            SPDLOG_ERROR("Exception while closing ClpArchiveReader: {}", ex.what());
        }
        m_archive_reader.reset();
    }
    m_event_count = 0;
    m_file_names.clear();
    m_file_infos.clear();
    m_tables.clear();
    m_pending_text_tables = decltype(m_pending_text_tables){};
    m_log_events.clear();
    m_active_decoded_text_buffer_idx = 1;
    m_pending_text_message.clear();
    m_pending_text_message_offset = 0;
    m_log_event_text_idx = 0;
    m_text_decode_done = false;
}

auto ClpArchiveReader::move_from(ClpArchiveReader& rhs) noexcept -> void {
    m_archive_reader = std::move(rhs.m_archive_reader);
    m_archive_data = std::move(rhs.m_archive_data);
    m_event_count = std::exchange(rhs.m_event_count, 0);
    m_file_names = std::move(rhs.m_file_names);
    m_file_infos = std::move(rhs.m_file_infos);
    m_tables = std::move(rhs.m_tables);
    m_pending_text_tables = std::move(rhs.m_pending_text_tables);
    m_log_events = std::move(rhs.m_log_events);
    m_decoded_text_buffers = std::move(rhs.m_decoded_text_buffers);
    m_active_decoded_text_buffer_idx = std::exchange(rhs.m_active_decoded_text_buffer_idx, 1);
    m_pending_text_message = std::move(rhs.m_pending_text_message);
    m_pending_text_message_offset = std::exchange(rhs.m_pending_text_message_offset, 0);
    m_log_event_text_idx = std::exchange(rhs.m_log_event_text_idx, 0);
    m_text_decode_done = std::exchange(rhs.m_text_decode_done, false);
}

auto ClpArchiveReader::get_num_log_events_per_schema() const -> std::vector<uint64_t> {
    std::vector<uint64_t> num_log_events_per_schema;
    num_log_events_per_schema.reserve(m_tables.size());
    for (auto const& table : m_tables) {
        num_log_events_per_schema.push_back(nullptr == table ? 0 : table->get_num_messages());
    }
    return num_log_events_per_schema;
}

auto ClpArchiveReader::decode_all() -> Result<std::vector<LogEvent>> {
    if (m_log_events.size() == m_event_count) {
        return m_log_events;
    }

    if (nullptr == m_archive_reader) {
        return SfaErrorCode{SfaErrorCodeEnum::NotInit};
    }

    try {
        m_log_events.clear();
        m_log_events.reserve(m_event_count);

        std::string message;
        int64_t timestamp{0};
        int64_t log_event_idx{0};
        while (true) {
            std::shared_ptr<clp_s::SchemaReader> next_table{nullptr};
            int64_t next_idx{0};

            for (auto const& table : m_tables) {
                if (nullptr == table) {
                    SPDLOG_ERROR("Failed to decode archive: encountered null schema table.");
                    return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
                }
                if (table->done()) {
                    continue;
                }
                auto const current_idx{table->get_next_log_event_idx()};
                if (nullptr == next_table || current_idx < next_idx) {
                    next_table = table;
                    next_idx = current_idx;
                }
            }

            if (nullptr == next_table) {
                break;
            }

            if (next_table->get_next_message_with_metadata(message, timestamp, log_event_idx)) {
                m_log_events.emplace_back(log_event_idx, timestamp, std::move(message));
            }
        }
        return m_log_events;
    } catch (std::bad_alloc const&) {
        SPDLOG_ERROR("Failed to decode archive: out of memory.");
        return SfaErrorCode{SfaErrorCodeEnum::NoMemory};
    } catch (std::exception const& ex) {
        SPDLOG_ERROR("Exception while decoding archive: {}", ex.what());
        return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
    }
}

auto ClpArchiveReader::decode_next_text_chunk() -> Result<uint64_t> {
    if (m_text_decode_done) {
        return 0;
    }

    if (nullptr == m_archive_reader) {
        return SfaErrorCode{SfaErrorCodeEnum::NotInit};
    }

    if (m_pending_text_tables.empty() && false == m_text_decode_done) {
        for (auto const& table : m_tables) {
            if (false == table->done()) {
                m_pending_text_tables.push(table);
            }
        }
    }

    m_active_decoded_text_buffer_idx = (m_active_decoded_text_buffer_idx + 1) % cNumChunkBuffers;
    auto* const buffer_start{
            m_decoded_text_buffers.data() + m_active_decoded_text_buffer_idx * cChunkBufferSize
    };
    size_t decoded_text_size{0};

    try {
        while (decoded_text_size < cChunkBufferSize) {
            if (m_pending_text_message.empty()) {
                if (m_pending_text_tables.empty()) {
                    m_text_decode_done = true;
                    break;
                }

                auto next_table = m_pending_text_tables.top();
                m_pending_text_tables.pop();

                if (false == next_table->get_next_message(m_pending_text_message)) {
                    return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
                }
                if (false == next_table->done()) {
                    m_pending_text_tables.push(next_table);
                }
                m_pending_text_message_offset = 0;
            }

            auto const remaining_message_size{
                    m_pending_text_message.size() - m_pending_text_message_offset
            };
            auto const remaining_chunk_capacity{cChunkBufferSize - decoded_text_size};
            auto const bytes_to_copy{
                    std::min(remaining_message_size, remaining_chunk_capacity)
            };
            std::memcpy(
                    buffer_start + decoded_text_size,
                    m_pending_text_message.data() + m_pending_text_message_offset,
                    bytes_to_copy
            );
            decoded_text_size += bytes_to_copy;
            m_pending_text_message_offset += bytes_to_copy;

            if (m_pending_text_message_offset == m_pending_text_message.size()) {
                m_pending_text_message.clear();
            }
        }

        return static_cast<uint64_t>(decoded_text_size);
    } catch (std::bad_alloc const&) {
        SPDLOG_ERROR("Failed to decode archive text: out of memory.");
        return SfaErrorCode{SfaErrorCodeEnum::NoMemory};
    } catch (std::exception const& ex) {
        SPDLOG_ERROR("Exception while decoding archive text: {}", ex.what());
        return SfaErrorCode{SfaErrorCodeEnum::IoFailure};
    }
}

auto ClpArchiveReader::precompute_archive_metadata() -> Result<void> {
    auto const& range_index{m_archive_reader->get_range_index()};
    m_file_names.reserve(range_index.size());
    m_file_infos.reserve(range_index.size());

    for (auto const& range : range_index) {
        auto const start_idx{static_cast<int64_t>(range.start_index)};
        auto const end_idx{static_cast<int64_t>(range.end_index)};
        m_event_count += static_cast<uint64_t>(end_idx - start_idx);

        auto const filename_it{
                range.fields.find(std::string{clp_s::constants::range_index::cFilename})
        };
        auto const filename{filename_it->get<std::string>()};

        m_file_names.push_back(filename);
        m_file_infos.emplace_back(filename, start_idx, end_idx);
    }

    m_archive_reader->read_dictionaries_and_metadata();
    m_archive_reader->open_packed_streams();
    m_tables = m_archive_reader->read_all_tables();
    std::erase(m_tables, nullptr);

    return ystdlib::error_handling::success();
}
}  // namespace clp_s::ffi::sfa
