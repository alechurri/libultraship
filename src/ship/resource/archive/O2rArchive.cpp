#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __PS4__
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace Ship {
#ifdef __PS4__
// Fallback for when libzip can't work on the file itself: read the whole archive and let libzip
// parse it from memory (it takes ownership of the buffer).
static zip_t* Ps4OpenZipFromMemory(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        SPDLOG_ERROR("[PS4] fopen(\"{}\") failed: {}", path, strerror(errno));
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        SPDLOG_ERROR("[PS4] \"{}\" reports a size of {} bytes", path, size);
        fclose(file);
        return nullptr;
    }

    void* data = malloc((size_t)size);
    if (data == nullptr) {
        SPDLOG_ERROR("[PS4] out of memory reading \"{}\" ({} bytes)", path, size);
        fclose(file);
        return nullptr;
    }

    const size_t bytesRead = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (bytesRead != (size_t)size) {
        SPDLOG_ERROR("[PS4] short read on \"{}\": {} of {} bytes", path, bytesRead, size);
        free(data);
        return nullptr;
    }

    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_buffer_create(data, (zip_uint64_t)size, 1, &error);
    if (source == nullptr) {
        SPDLOG_ERROR("[PS4] zip_source_buffer_create failed: {}", zip_error_strerror(&error));
        free(data);
        zip_error_fini(&error);
        return nullptr;
    }

    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        SPDLOG_ERROR("[PS4] zip_open_from_source(\"{}\") failed: {}", path, zip_error_strerror(&error));
        zip_source_free(source);
    } else {
        SPDLOG_INFO("[PS4] \"{}\" opened from memory ({} bytes)", path, size);
    }
    zip_error_fini(&error);
    return archive;
}
#endif

O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    if (mZipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    auto zipEntryIndex = zip_name_locate(mZipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(mZipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        return nullptr;
    }

    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

bool O2rArchive::Open() {
#ifdef __PS4__
    // Archives are never written on the console, and the one inside the package is on a read-only
    // file system.
    int zipError = 0;
    errno = 0;
    mZipArchive = zip_open(GetPath().c_str(), ZIP_RDONLY, &zipError);
    if (mZipArchive == nullptr) {
        SPDLOG_WARN("[PS4] zip_open(\"{}\") failed (libzip error {}, errno {}: {}), trying from memory", GetPath(),
                    zipError, errno, strerror(errno));
        mZipArchive = Ps4OpenZipFromMemory(GetPath());
    }
#else
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
#endif
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Cannot close zip file. Zip file not loaded. \"{}\"", GetPath());
        return false;
    }

    if (zip_close(mZipArchive) == -1) {
        SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = nullptr;
    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        return false;
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
}

} // namespace Ship
