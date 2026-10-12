#pragma once

#include <cstdint>
#include <functional>

namespace mtport {

struct TransferProgress {
    uint64_t bytesDone{};
    uint64_t bytesTotal{};
};

// Return false to request cancellation. Cancellation never reports success: the operation
// throws MtpError{Cancelled}.
using ProgressCallback = std::function<bool(const TransferProgress&)>;

}  // namespace mtport
