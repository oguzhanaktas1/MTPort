// Thin translation layer between the macFUSE libfuse 2.9-style high-level API and MtpFilesystem.
// All policy lives in MtpFilesystem; this file only converts types and exceptions to -errno.
#pragma once

#define FUSE_USE_VERSION 29
#define _FILE_OFFSET_BITS 64
#include <fuse.h>

#include <functional>

#include "mtport/MtpFilesystem.hpp"

namespace mtport::fuse_adapter {

// The filesystem the callbacks operate on. Set before fuse_main(); the adapter never owns it.
// (A global instead of fuse_get_context() keeps the callbacks callable without libfuse, so they
// can be unit-tested directly.)
void setFilesystem(MtpFilesystem* fs);

// Called from the FUSE init/destroy callbacks (worker uses them to publish "mounted").
struct Hooks {
    std::function<void()> onInit;
    std::function<void()> onDestroy;
};
void setHooks(Hooks hooks);

const struct fuse_operations& operations();

}  // namespace mtport::fuse_adapter
