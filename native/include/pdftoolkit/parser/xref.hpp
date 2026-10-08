#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pdftoolkit/errors.hpp"

namespace pdftoolkit::parser {

/// One cross-reference entry — where the bytes of indirect object `id`
/// live (audit task 2.2).
struct XRefEntry {
    enum class Kind : std::uint8_t {
        Free = 0,       ///< type-f entry (or never mentioned): not usable
        InUse = 1,      ///< type-n / stream type 1: `offset` holds the byte
                        ///< offset of the `N G obj` header
        Compressed = 2, ///< stream type 2: the object lives inside an
                        ///  object stream (ObjStm) — see objstm_object
    };

    Kind kind = Kind::Free;
    /// InUse: byte offset of the object header. Free: the next-free
    /// object number of the free chain (spec field 1 of type-f entries).
    std::uint64_t offset = 0;
    /// InUse/Free: the generation number.
    std::uint32_t generation = 0;
    /// Compressed: object number of the containing ObjStm.
    std::uint32_t objstm_object = 0;
    /// Compressed: index of the object within the ObjStm.
    std::uint32_t index_in_objstm = 0;
};

/// Dual-mode cross-reference index (audit task 2.2): a contiguous table
/// indexed by object id, built from classical plaintext `xref` tables
/// and/or PDF 1.5+ `/Type /XRef` streams (variable-width field
/// decoding), with an emergency linear scan as the corruption fallback.
///
/// Build policy (PDF spec, incremental updates):
///   * the section named by the LAST `startxref` is the newest;
///   * `/Prev` chains walk backwards in file history;
///   * an entry seen in a NEWER section always wins over an older one —
///     including a type-f entry (the object was deleted by the update);
///   * a broken or missing OLDER section only stops the chain (the
///     newer entries stay authoritative — real readers behave the same);
///   * a corrupt or unparseable NEWEST section triggers the emergency
///     linear scan for `N G obj` headers, which REPLACES the table and
///     is flagged via from_linear_scan(). The scan is byte-exact for
///     free-standing objects but cannot see objects compressed inside
///     object streams (their `N G obj` header does not exist in the
///     file) — the flag communicates the reduced trust.
///
/// FlateDecode limitation (honest, until audit task 2.4 lands): most
/// real PDF 1.5+ xref streams are Flate-compressed; this resolver
/// parses UNFILTERED xref streams only. A Flate-filtered newest section
/// degrades to the linear scan (flagged); when task 2.4's decompressor
/// arrives, the stream path becomes first-class (see problem P-017).
class XRefIndex {
public:
    /// Scans `bytes` (the whole document, typically MmapHandle::bytes())
    /// for the last startxref, resolves the xref chain it names and
    /// builds the lookup table. `tail_window` is locate_startxref's
    /// backward-scan window (default 1024, the audit's value — P-016).
    ///
    /// Throws PdfToolkitException:
    ///   empty bytes / zero window      -> InvalidArgument
    ///   no startxref (propagated)      -> UnreadablePdf
    ///   no usable entries at all (no
    ///   table and no `N G obj` found)  -> UnreadablePdf
    [[nodiscard]] static XRefIndex from_document(
        std::span<const std::uint8_t> bytes,
        std::size_t tail_window = 1024);

    /// Entry for `object`, or nullptr when `object >= size()`.
    /// A returned Free entry means "known but not usable" (deleted or
    /// never defined) — callers check `kind` before using `offset`.
    [[nodiscard]] const XRefEntry* find(std::uint32_t object) const noexcept;

    /// Number of object slots: ids `0 .. size()-1` are addressable
    /// (unmentioned ids read as Free).
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }

    /// True when the index was rebuilt by the emergency linear scan
    /// (corrupt or Flate-filtered newest section) — reduced-trust mode.
    [[nodiscard]] bool from_linear_scan() const noexcept {
        return from_linear_scan_;
    }

    XRefIndex() = default;
    XRefIndex(const XRefIndex&) = default;
    XRefIndex& operator=(const XRefIndex&) = default;
    XRefIndex(XRefIndex&&) noexcept = default;
    XRefIndex& operator=(XRefIndex&&) noexcept = default;

private:
    std::vector<XRefEntry> entries_;
    std::vector<std::uint8_t> written_;  // set-entry merge bitmap
    bool from_linear_scan_ = false;
};

}  // namespace pdftoolkit::parser
