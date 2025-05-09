#ifndef CLP_FFI_IR_STREAM_SEARCH_ERRORCODE_HPP
#define CLP_FFI_IR_STREAM_SEARCH_ERRORCODE_HPP

#include <cstdint>

#include <ystdlib/error_handling/ErrorCode.hpp>

namespace clp::ffi::ir_stream::search {
/**
 * Represents all possible error codes related to KV-pair IR stream search.
 */
enum class ErrorCodeEnum : uint8_t {
    EncodedTextAstDecodingFailure,
    LiteralTypeUnexpected,
    LiteralTypeUnsupported,
    MethodNotImplemented,
};

using ErrorCode = ystdlib::error_handling::ErrorCode<ErrorCodeEnum>;
}  // namespace clp::ffi::ir_stream::search

YSTDLIB_ERROR_HANDLING_MARK_AS_ERROR_CODE_ENUM(clp::ffi::ir_stream::search::ErrorCodeEnum);

#endif  // CLP_FFI_IR_STREAM_SEARCH_ERRORCODE_HPP
