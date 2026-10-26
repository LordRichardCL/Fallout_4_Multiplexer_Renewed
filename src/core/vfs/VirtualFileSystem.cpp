#include "vfs/VirtualFileSystem.hpp"

#include "util/Text.hpp"

#include <spdlog/spdlog.h>

namespace fo4mx {

uint32_t VirtualFileSystem::addRoot(FileSourceRoot root)
{
    roots_.push_back(std::move(root));
    return static_cast<uint32_t>(roots_.size() - 1);
}

void VirtualFileSystem::addFile(uint32_t root, const std::string& relativePath, uint64_t size)
{
    std::string key = normalizeAssetPath(relativePath);
    auto [it, inserted] = index_.try_emplace(key, static_cast<uint32_t>(files_.size()));
    if (inserted) {
        File f;
        f.relativePath = relativePath;
        for (auto& c : f.relativePath)
            if (c == '/')
                c = '\\';
        files_.push_back(std::move(f));
    }
    File& f = files_[it->second];
    f.sources.push_back(root);
    f.sizes.push_back(size);
}

void VirtualFileSystem::scan(ProgressSink& progress)
{
    for (uint32_t r = 0; r < roots_.size(); ++r) {
        progress.throwIfCancelled();
        progress.progress(r, roots_.size(), roots_[r].name);
        const auto& root = roots_[r].root;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            continue;
        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            auto rel = std::filesystem::relative(it->path(), root, ec);
            if (ec)
                continue;
            std::string relStr = pathToUtf8(rel);
            // MO2 metadata files at a mod's root are not part of Data.
            if (!roots_[r].baseGame && it.depth() == 0 && iequals(relStr, "meta.ini"))
                continue;
            addFile(r, relStr, it->file_size(ec));
        }
        if (ec)
            spdlog::warn("Error while scanning {}: {}", pathToUtf8(root), ec.message());
    }
    progress.progress(roots_.size(), roots_.size(), "done");
}

const VirtualFileSystem::File* VirtualFileSystem::find(std::string_view normalizedPath) const
{
    auto it = index_.find(std::string(normalizedPath));
    return it == index_.end() ? nullptr : &files_[it->second];
}

std::filesystem::path VirtualFileSystem::physicalPath(const File& f, size_t sourceSlot) const
{
    std::string rel = f.relativePath;
#ifndef _WIN32
    for (auto& c : rel)
        if (c == '\\')
            c = '/';
#endif
    return roots_[f.sources[sourceSlot]].root / utf8ToPath(rel);
}

std::vector<const VirtualFileSystem::File*> VirtualFileSystem::rootFiles() const
{
    std::vector<const File*> out;
    for (const auto& f : files_)
        if (f.relativePath.find('\\') == std::string::npos)
            out.push_back(&f);
    return out;
}

} // namespace fo4mx
