#include "mtport/FileHandleTable.hpp"

#include "mtport/PosixErrorMapper.hpp"

namespace mtport {

uint64_t FileHandleTable::insert(std::shared_ptr<OpenFile> file) {
    std::lock_guard lk(mutex_);
    const uint64_t id = next_++;
    files_.emplace(id, std::move(file));
    return id;
}

std::shared_ptr<OpenFile> FileHandleTable::get(uint64_t handle, uint64_t currentGeneration) const {
    std::lock_guard lk(mutex_);
    auto it = files_.find(handle);
    if (it == files_.end()) throw FsError(EBADF, "unknown file handle");
    if (it->second->generation != currentGeneration)
        throw FsError(ESTALE, "handle belongs to an earlier device session");
    return it->second;
}

std::shared_ptr<OpenFile> FileHandleTable::remove(uint64_t handle) {
    std::lock_guard lk(mutex_);
    auto it = files_.find(handle);
    if (it == files_.end()) return nullptr;
    auto f = std::move(it->second);
    files_.erase(it);
    return f;
}

std::vector<std::shared_ptr<OpenFile>> FileHandleTable::snapshot() const {
    std::lock_guard lk(mutex_);
    std::vector<std::shared_ptr<OpenFile>> out;
    out.reserve(files_.size());
    for (const auto& [id, f] : files_) out.push_back(f);
    return out;
}

size_t FileHandleTable::size() const {
    std::lock_guard lk(mutex_);
    return files_.size();
}

}  // namespace mtport
