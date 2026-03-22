#include "book_snapshot.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/orderbook/level2_book.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace itch {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string trim_session(const char* s10) noexcept {
    int len = 10;
    while (len > 0 && s10[len - 1] == ' ')
        --len;
    return std::string(s10, static_cast<std::size_t>(len));
}

static void replace_token(std::string& s,
                           std::string_view token,
                           std::string_view value) noexcept {
    for (std::size_t pos = 0;
         (pos = s.find(token, pos)) != std::string::npos; ) {
        s.replace(pos, token.size(), value);
        pos += value.size();
    }
}

// Insert `suffix` immediately before the last '.' extension, or append if
// no extension present.  E.g. "itch_SES_123.snap" + "_EOD" →
// "itch_SES_123_EOD.snap".
static std::string insert_suffix(std::string path,
                                  std::string_view suffix) noexcept {
    std::size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        path += suffix;
    else
        path.insert(dot, suffix);
    return path;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
BookSnapshot::BookSnapshot(std::string directory,
                            std::string filename_template) noexcept
    : dir_(std::move(directory)), tmpl_(std::move(filename_template)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        ITCH_LOG_ERROR("BookSnapshot: create_directories(%s) failed: %s",
                       dir_.c_str(), ec.message().c_str());
        return;
    }
    std::filesystem::permissions(dir_,
        std::filesystem::perms::owner_all   |
        std::filesystem::perms::group_read  | std::filesystem::perms::group_exec |
        std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ec);
    if (ec)
        ITCH_LOG_WARN("BookSnapshot: chmod(0755, %s) failed: %s",
                      dir_.c_str(), ec.message().c_str());

    ITCH_LOG_INFO("BookSnapshot: directory=%s template=%s",
                  dir_.c_str(), tmpl_.c_str());
}

// ---------------------------------------------------------------------------
// Path generation
// ---------------------------------------------------------------------------
std::string BookSnapshot::make_path(const char* session_10) const noexcept {
    uint64_t ts = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string filename = tmpl_;
    replace_token(filename, "{session}",   trim_session(session_10));
    replace_token(filename, "{timestamp}", std::to_string(ts));
    return dir_ + "/" + filename;
}

std::string BookSnapshot::path_for_session(const char* session_10) const noexcept {
    return make_path(session_10);
}

// ---------------------------------------------------------------------------
// Sidecar helpers
// ---------------------------------------------------------------------------
std::string BookSnapshot::latest_ptr_path() const noexcept {
    return dir_ + "/latest.ptr";
}

std::string BookSnapshot::latest_eod_ptr_path() const noexcept {
    return dir_ + "/latest_eod.ptr";
}

std::string BookSnapshot::read_ptr_file(const std::string& ptr_path) const noexcept {
    FILE* f = std::fopen(ptr_path.c_str(), "r");
    if (!f) return {};
    char buf[4096] = {};
    std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        --n;
    return std::string(buf, n);
}

bool BookSnapshot::write_ptr_file(const std::string& ptr_path,
                                   const std::string& snap_path) const noexcept {
    std::string tmp = ptr_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) {
        ITCH_LOG_ERROR("BookSnapshot: open(%s) failed: %s", tmp.c_str(), strerror(errno));
        return false;
    }
    std::fprintf(f, "%s\n", snap_path.c_str());
    std::fclose(f);
    if (std::rename(tmp.c_str(), ptr_path.c_str()) != 0) {
        ITCH_LOG_ERROR("BookSnapshot: rename(%s) failed: %s", ptr_path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Core serialisation/deserialisation
// ---------------------------------------------------------------------------
bool BookSnapshot::write_snapshot(const Level2Book* books, std::size_t count,
                                   const std::string& final_path) noexcept {
    std::string tmp_path = final_path + ".tmp";
    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (!f) {
        ITCH_LOG_ERROR("BookSnapshot: open(%s) failed: %s",
                       tmp_path.c_str(), strerror(errno));
        return false;
    }

    const uint32_t magic = 0x49544348; // 'ITCH'
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&count, sizeof(count), 1, f);

    std::vector<uint8_t> buf(4096);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t written = books[i].serialize_snapshot(buf.data(), buf.size());
        if (written == 0) {
            buf.resize(buf.size() * 2);
            written = books[i].serialize_snapshot(buf.data(), buf.size());
        }
        uint32_t len = static_cast<uint32_t>(written);
        std::fwrite(&len, 4, 1, f);
        std::fwrite(buf.data(), 1, written, f);
    }

    std::fclose(f);

    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ITCH_LOG_ERROR("BookSnapshot: rename(%s → %s) failed: %s",
                       tmp_path.c_str(), final_path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

bool BookSnapshot::read_snapshot(Level2Book* books, std::size_t count,
                                  const std::string& path) noexcept {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        ITCH_LOG_WARN("BookSnapshot: cannot open snapshot %s: %s",
                      path.c_str(), strerror(errno));
        return false;
    }

    uint32_t magic = 0;
    std::fread(&magic, 4, 1, f);
    if (magic != 0x49544348) {
        ITCH_LOG_ERROR("BookSnapshot: bad magic in %s", path.c_str());
        std::fclose(f);
        return false;
    }

    std::size_t saved_count = 0;
    std::fread(&saved_count, sizeof(saved_count), 1, f);
    if (saved_count != count)
        ITCH_LOG_WARN("BookSnapshot: count mismatch (saved=%zu expected=%zu)",
                      saved_count, count);

    std::vector<uint8_t> buf(4096);
    std::size_t to_restore = std::min(saved_count, count);
    for (std::size_t i = 0; i < to_restore; ++i) {
        uint32_t len = 0;
        std::fread(&len, 4, 1, f);
        if (len > buf.size()) buf.resize(len);
        std::fread(buf.data(), 1, len, f);
        books[i].restore_snapshot(buf.data(), len);
    }

    std::fclose(f);
    ITCH_LOG_INFO("BookSnapshot: restored %zu books from %s", to_restore, path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// save — intra-day interval snapshot
// ---------------------------------------------------------------------------
bool BookSnapshot::save(const Level2Book* books, std::size_t count,
                         const char* session_10) noexcept {
    std::string final_path = make_path(session_10);
    if (!write_snapshot(books, count, final_path))
        return false;

    ITCH_LOG_INFO("BookSnapshot: saved %zu books to %s", count, final_path.c_str());
    write_ptr_file(latest_ptr_path(), final_path);
    return true;
}

// ---------------------------------------------------------------------------
// save_eod — EOD archival snapshot
//
// Appends "_EOD" before the extension so EOD files are easily identified.
// Updates both latest.ptr (so load() finds it on restart) and
// latest_eod.ptr (so pre-open warmup can find it independently).
// ---------------------------------------------------------------------------
bool BookSnapshot::save_eod(const Level2Book* books, std::size_t count,
                              const char* session_10) noexcept {
    std::string base_path  = make_path(session_10);
    std::string final_path = insert_suffix(base_path, "_EOD");

    if (!write_snapshot(books, count, final_path))
        return false;

    ITCH_LOG_INFO("BookSnapshot: EOD archived %zu books to %s", count, final_path.c_str());
    write_ptr_file(latest_ptr_path(),     final_path);
    write_ptr_file(latest_eod_ptr_path(), final_path);
    return true;
}

// ---------------------------------------------------------------------------
// load — restore from latest.ptr
// ---------------------------------------------------------------------------
bool BookSnapshot::load(Level2Book* books, std::size_t count) noexcept {
    std::string path = read_ptr_file(latest_ptr_path());
    if (path.empty()) {
        ITCH_LOG_INFO("BookSnapshot: no latest.ptr in %s — skipping restore", dir_.c_str());
        return false;
    }
    return read_snapshot(books, count, path);
}

// ---------------------------------------------------------------------------
// load_eod — restore from latest_eod.ptr (pre-open warmup path)
// ---------------------------------------------------------------------------
bool BookSnapshot::load_eod(Level2Book* books, std::size_t count) noexcept {
    std::string path = read_ptr_file(latest_eod_ptr_path());
    if (path.empty()) {
        ITCH_LOG_INFO("BookSnapshot: no latest_eod.ptr in %s — skipping warmup",
                      dir_.c_str());
        return false;
    }
    ITCH_LOG_INFO("BookSnapshot: loading EOD snapshot for pre-open warmup: %s",
                  path.c_str());
    return read_snapshot(books, count, path);
}

} // namespace itch
