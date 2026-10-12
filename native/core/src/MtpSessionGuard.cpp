#include "MtpSessionGuard.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace mtport::detail {

void ensureLibmtpInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        LIBMTP_Init();
        LIBMTP_Set_Debug(0);  // LIBMTP_DEBUG_NONE: libmtp debug output may contain file names
    });
}

StdoutToStderr::StdoutToStderr() {
    std::fflush(stdout);
    saved_ = ::dup(STDOUT_FILENO);
    if (saved_ >= 0) ::dup2(STDERR_FILENO, STDOUT_FILENO);
}

StdoutToStderr::~StdoutToStderr() {
    std::fflush(stdout);  // push libmtp's buffered text out while it still goes to stderr
    if (saved_ >= 0) {
        ::dup2(saved_, STDOUT_FILENO);
        ::close(saved_);
    }
}

std::string takeString(char* s) {
    if (!s) return {};
    std::string out(s);
    LIBMTP_FreeMemory(s);
    return out;
}

RawFileList RawFileList::adopt(LIBMTP_file_t* head) {
    RawFileList out;
    while (head) {
        LIBMTP_file_t* next = head->next;
        head->next = nullptr;  // LIBMTP_destroy_file_t frees only this node
        out.nodes.emplace_back(head);
        head = next;
    }
    return out;
}

}  // namespace mtport::detail
