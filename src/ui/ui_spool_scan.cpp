// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_scan.h"

#include "ui_ams_context_menu.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_mapper.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace helix::ui {

namespace {

lv_subject_t s_scan_button{};
bool s_scan_button_initialized = false;
SubjectManager s_subjects;

ConfirmOptions cancel_labelled(const char* text) {
    ConfirmOptions options;
    options.cancel_text = text;
    return options;
}

bool same_text(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/// "Creality PLA #FF0000", leaving out whatever is unknown.
std::string describe(const std::string& brand, const std::string& material,
                     std::optional<uint32_t> color) {
    std::string out = brand;
    if (!material.empty()) {
        out += out.empty() ? material : " " + material;
    }
    if (color) {
        out += fmt::format("{}#{:06X}", out.empty() ? "" : " ", *color & 0xFFFFFF);
    }
    return out.empty() ? std::string(lv_tr("nothing")) : out;
}

std::string describe_slot(const SlotInfo& slot) {
    std::optional<uint32_t> color;
    if (helix::ams::is_declarable_color(slot.color_rgb)) {
        color = slot.color_rgb;
    }
    return describe(slot.brand, slot.material, color);
}

std::string describe_tag(const helix::ams::Observation& tag) {
    return describe(tag.brand.value_or(""), tag.material.value_or(""), tag.color_rgb);
}

/// Repoint the slot's linked Spoolman spool at a filament matching the tag,
/// then commit the result to the slot.
void update_spoolman_from_tag(int slot_index, helix::ams::Observation tag) {
    AmsBackend* backend = AmsState::instance().get_backend();
    IMoonrakerAPI* api = get_moonraker_api();
    if (!backend || !api) {
        return;
    }
    const SlotInfo original = backend->get_slot_info(slot_index);
    const SlotInfo edited = apply_tag_to_slot(original, tag);
    auto saver = std::make_shared<helix::SpoolmanSlotSaver>(
        api, get_printer_state().get_discovery().filament_diameter_mm());
    saver->save(
        original, edited, helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked,
        [saver, slot_index, original, edited](const helix::SaveResult& result) {
            // Spoolman answers on a background thread.
            helix::ui::queue_update("spool_scan::update_done", [slot_index, original, edited,
                                                                result]() {
                if (result.missing.any()) {
                    ToastManager::instance().show(
                        ToastSeverity::WARNING,
                        lv_tr("The tag lacks a brand, material or colour Spoolman needs"), 4000);
                    return;
                }
                if (!result.success) {
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        lv_tr("Couldn't save to Spoolman. Nothing changed on this printer."), 3000);
                    return;
                }
                SlotInfo committed = edited;
                if (result.new_filament_id != 0) {
                    committed.spoolman_filament_id = result.new_filament_id;
                }
                if (result.new_vendor_id != 0) {
                    committed.spoolman_vendor_id = result.new_vendor_id;
                }
                AmsError err =
                    AmsState::instance().commit_slot_edit(slot_index, original, committed);
                if (!err.success()) {
                    notify_ams_error(err);
                    return;
                }
                SpoolmanManager::refresh_spool(committed.spoolman_id);
                NOTIFY_INFO(lv_tr("Spoolman spool #{} updated from the tag"),
                            committed.spoolman_id);
            });
        });
}

/// What a finished read means for @p slot_index, said to the user.
void report_tag_read(int slot_index) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const SlotInfo slot = backend->get_slot_info(slot_index);
    const std::optional<helix::ams::Observation> tag =
        helix::ams::lane_sources(backend->lane_id(slot_index)).vendor_cache;
    const TagComparison cmp = compare_tag_to_slot(tag, slot);
    const std::string lane = lane_label(backend->lane_noun(), slot_index);

    if (cmp.outcome == TagComparison::Outcome::NoReading) {
        NOTIFY_WARNING(lv_tr("{}: no tag read"), lane);
        return;
    }

    const bool has_spoolman = lv_subject_get_int(get_printer_state().capabilities_state().subject(
                                  helix::Capability::HasSpoolman)) == 1;
    if (!has_spoolman) {
        NOTIFY_INFO(lv_tr("{}: tag read"), lane);
        return;
    }

    if (slot.spoolman_id <= 0) {
        const std::string message = fmt::format(
            lv_tr("{} reads {}. Choose the matching Spoolman spool?"), lane, describe_tag(*tag));
        modal_confirm(
            lv_tr("Link to Spoolman?"), message.c_str(), ModalSeverity::Info, lv_tr("Choose Spool"),
            [slot_index]() {
                open_slot_editor(lv_screen_active(), get_moonraker_api(), slot_index,
                                 /*open_on_picker=*/true);
            },
            cancel_labelled(lv_tr("Not Now")));
        return;
    }

    if (cmp.outcome == TagComparison::Outcome::Matches) {
        NOTIFY_INFO(lv_tr("{} matches Spoolman spool #{}"), lane, slot.spoolman_id);
        return;
    }

    const std::string message =
        fmt::format(lv_tr("{} reads {}, but Spoolman spool #{} is {}. Update the spool in "
                          "Spoolman to match the tag?"),
                    lane, describe_tag(*tag), slot.spoolman_id, describe_slot(slot));
    modal_confirm(
        lv_tr("Tag Doesn't Match Spoolman"), message.c_str(), ModalSeverity::Warning,
        lv_tr("Update Spoolman"),
        [slot_index, tag = *tag]() { update_spoolman_from_tag(slot_index, tag); },
        cancel_labelled(lv_tr("Keep")));
}

} // namespace

ScanButton resolve_scan_button(SpoolScanMethod method, bool reads_tags, bool has_spoolman,
                               bool qr_available) {
    const ScanButton qr = has_spoolman && qr_available ? ScanButton::QrCode : ScanButton::None;
    switch (method) {
    case SpoolScanMethod::Off:
        return ScanButton::None;
    case SpoolScanMethod::QrCode:
        return qr;
    case SpoolScanMethod::Auto:
    case SpoolScanMethod::Rfid:
        return reads_tags ? ScanButton::Rfid : qr;
    }
    return ScanButton::None;
}

void init_scan_button_subject() {
    if (s_scan_button_initialized) {
        return;
    }
    lv_subject_init_int(&s_scan_button, static_cast<int>(ScanButton::None));
    s_subjects.publish("ams_scan_button", &s_scan_button);
    s_scan_button_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("SpoolScanButton", []() {
        if (s_scan_button_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_scan_button_initialized = false;
        }
    });
}

ScanButton refresh_scan_button(const AmsBackend* backend) {
    init_scan_button_subject();
    const bool has_spoolman = lv_subject_get_int(get_printer_state().capabilities_state().subject(
                                  helix::Capability::HasSpoolman)) == 1;
#if defined(HELIX_PLATFORM_ESP32)
    constexpr bool qr_available = false; // no camera on the v1 Core+AMS cut
#else
    constexpr bool qr_available = true;
#endif
    const ScanButton button = resolve_scan_button(
        SettingsManager::instance().get_ams_spool_scan_method(),
        backend != nullptr && backend->supports_tag_read(), has_spoolman, qr_available);
    lv_subject_set_int(&s_scan_button, static_cast<int>(button));
    return button;
}

TagComparison compare_tag_to_slot(const std::optional<helix::ams::Observation>& tag,
                                  const SlotInfo& slot) {
    TagComparison cmp;
    if (!tag || (!tag->material && !tag->color_rgb)) {
        return cmp;
    }
    if (tag->material && !tag->material->empty() && !slot.material.empty()) {
        cmp.material_differs = !FilamentMapper::materials_match(*tag->material, slot.material);
    }
    if (tag->color_rgb && helix::ams::is_declarable_color(slot.color_rgb)) {
        cmp.color_differs = !FilamentMapper::colors_match(*tag->color_rgb, slot.color_rgb);
    }
    if (tag->brand && !tag->brand->empty() && !slot.brand.empty()) {
        cmp.brand_differs = !same_text(*tag->brand, slot.brand);
    }
    cmp.outcome = cmp.material_differs || cmp.color_differs || cmp.brand_differs
                      ? TagComparison::Outcome::Differs
                      : TagComparison::Outcome::Matches;
    return cmp;
}

SlotInfo apply_tag_to_slot(const SlotInfo& slot, const helix::ams::Observation& tag) {
    SlotInfo out = slot;
    if (tag.material && !tag.material->empty()) {
        out.material = *tag.material;
    }
    if (tag.color_rgb) {
        out.color_rgb = *tag.color_rgb;
    }
    if (tag.brand && !tag.brand->empty()) {
        out.brand = *tag.brand;
    }
    return out;
}

void scan_slot_tag(int slot_index) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const std::string lane = lane_label(backend->lane_noun(), slot_index);
    AmsError err = backend->read_slot_tag(
        slot_index, [slot_index](const AmsError& result, std::vector<int> /*slots*/) {
            if (!result.success()) {
                notify_ams_error(result, lv_tr("Tag read failed"));
                return;
            }
            report_tag_read(slot_index);
        });
    if (!err.success()) {
        notify_ams_error(err, lv_tr("Tag read failed"));
        return;
    }
    NOTIFY_INFO(lv_tr("Reading the tag in {}…"), lane);
}

} // namespace helix::ui
