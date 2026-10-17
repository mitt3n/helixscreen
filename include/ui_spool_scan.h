// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_observation.h"
#include "settings_manager.h"

#include <optional>

namespace helix {
class AmsBackend;
} // namespace helix

namespace helix::ui {

/// The one scan button the slot menu and slot editor show. Published as the
/// integer subject "ams_scan_button"; each button binds to one value.
enum class ScanButton { None = 0, QrCode = 1, Rfid = 2 };

/// Which scan button to show.
///
/// @param method       The per-printer Spool Scan Button setting.
/// @param reads_tags   AmsBackend::supports_tag_read() for the surface's backend;
///                     false for a spool no reader can see (the bypass spool).
/// @param has_spoolman Spoolman is reachable. A QR code only names a Spoolman spool.
/// @param qr_available The build can scan a QR code at all.
///
/// Pure.
[[nodiscard]] ScanButton resolve_scan_button(SpoolScanMethod method, bool reads_tags,
                                             bool has_spoolman, bool qr_available);

/// Register "ams_scan_button". Idempotent; deinit rides StaticSubjectRegistry.
void init_scan_button_subject();

/// Recompute "ams_scan_button" from live state for a surface about to show.
/// @param backend The surface's backend, or nullptr for a spool it has no
///        reader for.
ScanButton refresh_scan_button(const AmsBackend* backend);

/// How a tag reading compares with what a slot currently shows.
struct TagComparison {
    enum class Outcome {
        NoReading, ///< The reader stated neither a material nor a colour.
        Matches,   ///< Every field the reader stated agrees with the slot.
        Differs,   ///< At least one stated field disagrees.
    };
    Outcome outcome = Outcome::NoReading;
    bool material_differs = false;
    bool color_differs = false;
    bool brand_differs = false;
};

/// Compare what the filament system read (the lane's VendorCache record) with
/// the slot as shown, which is the linked Spoolman spool when there is one.
/// A field either side leaves unstated is not compared.
///
/// Pure.
[[nodiscard]] TagComparison compare_tag_to_slot(const std::optional<helix::ams::Observation>& tag,
                                                const SlotInfo& slot);

/// @p slot with every field @p tag states written over it.
[[nodiscard]] SlotInfo apply_tag_to_slot(const SlotInfo& slot, const helix::ams::Observation& tag);

/// Re-read the tag in @p slot_index on the active backend, then compare it
/// with the slot's Spoolman spool and offer to update that spool when they
/// disagree. Main thread.
void scan_slot_tag(int slot_index);

} // namespace helix::ui
