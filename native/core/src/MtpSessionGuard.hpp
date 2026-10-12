// RAII ownership of libmtp handles (private to the libmtp backend).
#pragma once

#include <libmtp.h>

#include <memory>
#include <string>
#include <vector>

#include "mtport/MtpError.hpp"

namespace mtport::detail {

void ensureLibmtpInit();

// libmtp prints chatter ("Device 0 (VID=...) is a ...") to stdout. That would corrupt --json output and
// any pipe, so stdout is pointed at stderr for the duration of a libmtp call.
class StdoutToStderr {
public:
    StdoutToStderr();
    ~StdoutToStderr();
    StdoutToStderr(const StdoutToStderr&) = delete;
    StdoutToStderr& operator=(const StdoutToStderr&) = delete;

private:
    int saved_{-1};
};  // LIBMTP_Init exactly once, debug output off

struct SessionDeleter {
    void operator()(LIBMTP_mtpdevice_t* d) const noexcept {
        if (d) LIBMTP_Release_Device(d);
    }
};
using SessionPtr = std::unique_ptr<LIBMTP_mtpdevice_t, SessionDeleter>;

struct FileDeleter {
    void operator()(LIBMTP_file_t* f) const noexcept {
        if (f) LIBMTP_destroy_file_t(f);
    }
};
using FilePtr = std::unique_ptr<LIBMTP_file_t, FileDeleter>;

struct MemDeleter {
    void operator()(void* p) const noexcept {
        if (p) LIBMTP_FreeMemory(p);
    }
};
template <class T>
using MemPtr = std::unique_ptr<T, MemDeleter>;

// Copies and frees a libmtp-allocated C string (nullptr -> "").
std::string takeString(char* s);

// Frees a linked list returned by LIBMTP_Get_Files_And_Folders node by node, converting.
struct RawFileList {
    std::vector<FilePtr> nodes;  // each node detached from the chain, owned individually
    static RawFileList adopt(LIBMTP_file_t* head);
};

}  // namespace mtport::detail
