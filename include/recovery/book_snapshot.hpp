#pragma once
#include "../common/types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace itch {

class Level2Book;

// -----------------------------------------------------------------
// BookSnapshot — serialises/deserialises Level2Book state.
//
// Binary format (unchanged):
//   uint32_t magic  = 0x49544348  ('ITCH')
//   size_t   count
//   for each book: uint32_t len + len bytes from serialize_snapshot()
//
// Filename is derived from the configured template by substituting:
//   {session}   → MoldUDP64 session, trailing ASCII spaces stripped
//   {timestamp} → steady_clock nanoseconds (process-relative, unique
//                 within a run; different across reboots)
//
// Atomic write: write to <path>.tmp → fsync → rename(<path>).
// A "latest.ptr" sidecar always holds the path of the most recently
// committed snapshot so load() can find it without knowing the session.
// -----------------------------------------------------------------
class BookSnapshot {
  public:
    // directory: created (mode 0755) by the constructor if absent.
    // filename_template: tokens {session} and {timestamp} are expanded.
    BookSnapshot(std::string directory, std::string filename_template) noexcept;

    // Serialise books to a new uniquely-named file.
    // session_10: raw 10-byte (non-null-terminated) MoldUDP64 session.
    bool save(const Level2Book* books, std::size_t count,
              const char* session_10) noexcept;

    // EOD archival: same as save() but appends "_EOD" to the filename
    // (before the extension, if any) so EOD snapshots are distinguishable
    // from intra-day interval snapshots.
    // Updates latest.ptr and also writes a "latest_eod.ptr" sidecar so
    // the pre-open warmup path can locate the most recent EOD snapshot
    // independently of intra-day saves.
    bool save_eod(const Level2Book* books, std::size_t count,
                  const char* session_10) noexcept;

    // Restore from the snapshot named in latest.ptr.
    bool load(Level2Book* books, std::size_t count) noexcept;

    // Restore from the snapshot named in latest_eod.ptr.
    // Used by pre-open warmup to load prior-day close prices.
    bool load_eod(Level2Book* books, std::size_t count) noexcept;

    // Expands the template for session_10 at the current instant.
    // Used for logging / monitoring; save() calls this internally.
    std::string path_for_session(const char* session_10) const noexcept;

  private:
    std::string dir_;
    std::string tmpl_;

    // Expand {session} and {timestamp} in tmpl_, prepend dir_.
    std::string make_path(const char* session_10) const noexcept;

    // Core serialisation/deserialisation — shared by save() and save_eod().
    bool write_snapshot(const Level2Book* books, std::size_t count,
                        const std::string& final_path) noexcept;
    bool read_snapshot(Level2Book* books, std::size_t count,
                       const std::string& path) noexcept;

    // Helpers for the latest.ptr / latest_eod.ptr sidecars.
    std::string latest_ptr_path() const noexcept;
    std::string latest_eod_ptr_path() const noexcept;
    std::string read_ptr_file(const std::string& ptr_path) const noexcept;
    bool        write_ptr_file(const std::string& ptr_path,
                               const std::string& snap_path) const noexcept;
};

} // namespace itch
