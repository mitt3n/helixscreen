// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spool_scan.cpp
 * @brief The Spool Scan Button setting, which scan button it shows, and how a
 *        tag reading compares with what a slot shows.
 */

#include "ui_spool_scan.h"

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "lane_translation.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using helix::SlotInfo;
using helix::SpoolScanMethod;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ui::ScanButton;
using helix::ui::TagComparison;

TEST_CASE_METHOD(LVGLTestFixture, "ams spool_scan_method setting", "[settings][ams][spool_scan]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    const std::string path = config->df() + "ams/spool_scan_method";

    SECTION("defaults to Auto") {
        CHECK(settings.get_ams_spool_scan_method() == SpoolScanMethod::Auto);
        CHECK(lv_subject_get_int(settings.subject_ams_spool_scan_method()) == 1);
    }

    SECTION("each value round-trips and persists per printer") {
        for (auto method : {SpoolScanMethod::Off, SpoolScanMethod::QrCode, SpoolScanMethod::Rfid,
                            SpoolScanMethod::Auto}) {
            settings.set_ams_spool_scan_method(method);
            CHECK(settings.get_ams_spool_scan_method() == method);
            CHECK(config->get<int>(path, -1) == static_cast<int>(method));
            CHECK(lv_subject_get_int(settings.subject_ams_spool_scan_method()) ==
                  static_cast<int>(method));
        }
    }
}

TEST_CASE("resolve_scan_button picks the one button to show", "[ams][spool_scan]") {
    using helix::ui::resolve_scan_button;
    // (method, reads_tags, has_spoolman, qr_available)

    SECTION("Off shows nothing, whatever is available") {
        CHECK(resolve_scan_button(SpoolScanMethod::Off, true, true, true) == ScanButton::None);
    }

    SECTION("Auto reads the tag where a reader exists") {
        CHECK(resolve_scan_button(SpoolScanMethod::Auto, true, true, true) == ScanButton::Rfid);
        CHECK(resolve_scan_button(SpoolScanMethod::Auto, true, false, false) == ScanButton::Rfid);
    }

    SECTION("Auto and RFID fall back to QR where there is no reader") {
        CHECK(resolve_scan_button(SpoolScanMethod::Auto, false, true, true) == ScanButton::QrCode);
        CHECK(resolve_scan_button(SpoolScanMethod::Rfid, false, true, true) == ScanButton::QrCode);
    }

    SECTION("QR always means QR, even with a reader") {
        CHECK(resolve_scan_button(SpoolScanMethod::QrCode, true, true, true) == ScanButton::QrCode);
    }

    SECTION("QR needs Spoolman and a build that can scan") {
        CHECK(resolve_scan_button(SpoolScanMethod::QrCode, true, false, true) == ScanButton::None);
        CHECK(resolve_scan_button(SpoolScanMethod::QrCode, true, true, false) == ScanButton::None);
        CHECK(resolve_scan_button(SpoolScanMethod::Auto, false, false, true) == ScanButton::None);
    }
}

namespace {
SlotInfo slot_of(const std::string& brand, const std::string& material, uint32_t color) {
    SlotInfo s;
    s.brand = brand;
    s.material = material;
    s.color_rgb = color;
    return s;
}

Observation tag_of(std::optional<std::string> brand, std::optional<std::string> material,
                   std::optional<uint32_t> color) {
    Observation o(ObservationSource::VendorCache);
    o.brand = std::move(brand);
    o.material = std::move(material);
    o.color_rgb = color;
    return o;
}
} // namespace

TEST_CASE("compare_tag_to_slot", "[ams][spool_scan]") {
    using helix::ui::compare_tag_to_slot;
    const SlotInfo slot = slot_of("Creality", "PLA", 0xFF0000);

    SECTION("no record, or one stating neither material nor colour, is no reading") {
        CHECK(compare_tag_to_slot(std::nullopt, slot).outcome == TagComparison::Outcome::NoReading);
        CHECK(compare_tag_to_slot(tag_of("Creality", std::nullopt, std::nullopt), slot).outcome ==
              TagComparison::Outcome::NoReading);
    }

    SECTION("the same spool matches, case and close colour aside") {
        const auto cmp = compare_tag_to_slot(tag_of("CREALITY", "pla", 0xFA0505), slot);
        CHECK(cmp.outcome == TagComparison::Outcome::Matches);
    }

    SECTION("each disagreeing field is named") {
        const auto material = compare_tag_to_slot(tag_of("Creality", "PETG", 0xFF0000), slot);
        CHECK(material.outcome == TagComparison::Outcome::Differs);
        CHECK(material.material_differs);
        CHECK_FALSE(material.color_differs);
        CHECK_FALSE(material.brand_differs);

        const auto color = compare_tag_to_slot(tag_of("Creality", "PLA", 0x0000FF), slot);
        CHECK(color.outcome == TagComparison::Outcome::Differs);
        CHECK(color.color_differs);
        CHECK_FALSE(color.material_differs);

        const auto brand = compare_tag_to_slot(tag_of("Polymaker", "PLA", 0xFF0000), slot);
        CHECK(brand.outcome == TagComparison::Outcome::Differs);
        CHECK(brand.brand_differs);
    }

    SECTION("a field either side leaves unstated is not compared") {
        CHECK(compare_tag_to_slot(tag_of(std::nullopt, "PLA", std::nullopt), slot).outcome ==
              TagComparison::Outcome::Matches);
        const SlotInfo blank = slot_of("", "", helix::AMS_DEFAULT_SLOT_COLOR);
        CHECK(compare_tag_to_slot(tag_of("Creality", "PETG", 0x00FF00), blank).outcome ==
              TagComparison::Outcome::Matches);
    }
}

TEST_CASE("apply_tag_to_slot writes only what the tag states", "[ams][spool_scan]") {
    SlotInfo slot = slot_of("Polymaker", "PETG", 0x0000FF);
    slot.spoolman_id = 12;

    const SlotInfo out = helix::ui::apply_tag_to_slot(slot, tag_of(std::nullopt, "PLA", 0xFF0000));
    CHECK(out.material == "PLA");
    CHECK(out.color_rgb == 0xFF0000);
    CHECK(out.brand == "Polymaker");
    CHECK(out.spoolman_id == 12);
}
