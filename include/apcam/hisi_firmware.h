#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Validation of Hi3516DV500 vendor update images before they reach the rootfs
// updater, matching tools/hisi_firmware.py check_appfs_image(). The web
// UI only forwards appfs: one entry for the appfs partition that reboots after
// writing, so an upload can never rewrite the rootfs that holds the recovery
// updater.
namespace apcam_hisi_firmware {

constexpr uint32_t MAGIC = 0xabcabccbU;
constexpr uint32_t MAGIC_END = 0xcbcbacbaU;
constexpr uint32_t TYPE_MTD = 0x42U;
constexpr size_t HEADER_SIZE = 0x34;
constexpr size_t ENTRY_SIZE = 0xa4;
constexpr size_t TRAILER_SIZE = 0x1c;
constexpr size_t ENTRY_PATH_SIZE = 0x80;
constexpr const char *APPFS_MTD = "/dev/mtd4";
constexpr size_t PEB_SIZE = 0x20000;
constexpr size_t VID_HDR_OFFSET = 0x800;
constexpr size_t DATA_OFFSET = 0x1000;
constexpr size_t APPFS_PEBS = 288;
// The volume size this camera's UBI and the vendor's own updates give appfs.
constexpr size_t APPFS_VOLUME_MAX_PEBS = 265;
constexpr size_t VTBL_SLOTS = 128;
constexpr size_t VTBL_RECORD_SIZE = 172;

// Upload names: .img as the vendor web UI requires, or .bin, in the characters
// vendor releases and browser renames use ("IPC_..._V16(260305) (1).img").
// upgrade.js applies NAME_REGEX; both must agree.
constexpr size_t NAME_MAX_LENGTH = 200;
constexpr const char *NAME_REGEX = "^[A-Za-z0-9._()+ -]{1,196}\\.(img|bin)$";

inline bool valid_image_name(const char *name)
{
    const size_t length = std::strlen(name);
    if (length < 5 || length > NAME_MAX_LENGTH) return false;
    if (std::strcmp(name + length - 4, ".img") != 0 && std::strcmp(name + length - 4, ".bin") != 0)
        return false;
    for (size_t i = 0; i < length; i++) {
        const char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              std::strchr("._()+ -", c)))
            return false;
    }
    return true;
}

// updatetype.ini [UPDATE_SET] DEV_TYPE / SDK_TYPE / PLATFORM_TYPE.
struct device_code { uint32_t dev, sdk, platform; };
constexpr device_code CAMERA_DEVICE_CODE = {19, 2, 51};

// Vendor firmware lines lay the flash out differently; the device code the
// updater checks tells them apart. The U35.5 line keeps appfs as a UBI volume
// in a 36 MiB mtd4; the MAIN line keeps it as a raw squashfs in a 48 MiB mtd5,
// with fconfigs in mtd4.
struct appfs_layout {
    device_code code;
    const char *mtd;
    size_t pebs;
    bool ubi;
};
constexpr appfs_layout APPFS_LAYOUTS[] = {
    {{19, 2, 51}, "/dev/mtd4", APPFS_PEBS, true},
    {{19, 2, 35}, "/dev/mtd5", 384, false},
};
constexpr size_t MAX_APPFS_PEBS = 384;
constexpr size_t MAX_IMAGE_SIZE = HEADER_SIZE + ENTRY_SIZE + MAX_APPFS_PEBS * PEB_SIZE + TRAILER_SIZE;

inline const appfs_layout *find_layout(device_code code)
{
    for (const appfs_layout &layout : APPFS_LAYOUTS) {
        if (layout.code.dev == code.dev && layout.code.sdk == code.sdk && layout.code.platform == code.platform)
            return &layout;
    }
    return nullptr;
}

inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t be32(const uint8_t *p)
{
    return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
}

// Reflected table CRC-32: the vendor uses poly 0xbdb083e0 seeded with 0xcb;
// UBI uses 0xedb88320 seeded with ~0 and, like the vendor, no final xor.
inline uint32_t reflected_crc(uint32_t poly, uint32_t crc, const uint8_t *data, size_t length)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; bit++) value = (value & 1U) ? (value >> 1) ^ poly : value >> 1;
        table[i] = value;
    }
    for (size_t i = 0; i < length; i++) crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
    return crc;
}
inline uint32_t vendor_crc(const uint8_t *data, size_t length)
{
    return reflected_crc(0xbdb083e0U, 0xcbU, data, length);
}
inline uint32_t ubi_crc(const uint8_t *data, size_t length)
{
    return reflected_crc(0xedb88320U, 0xffffffffU, data, length);
}

// Reads DEV_TYPE, SDK_TYPE and PLATFORM_TYPE; false leaves *code unchanged.
inline bool read_device_code(const char *path, device_code *code)
{
    FILE *file = std::fopen(path, "r");
    if (!file) return false;
    device_code found = {0, 0, 0};
    unsigned seen = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned value;
        if (std::sscanf(line, " DEV_TYPE = %u", &value) == 1) { found.dev = value; seen |= 1U; }
        else if (std::sscanf(line, " SDK_TYPE = %u", &value) == 1) { found.sdk = value; seen |= 2U; }
        else if (std::sscanf(line, " PLATFORM_TYPE = %u", &value) == 1) { found.platform = value; seen |= 4U; }
    }
    std::fclose(file);
    if (seen != 7U) return false;
    *code = found;
    return true;
}

inline const char *check_ubi(const uint8_t *image, size_t length, char *error, size_t error_size)
{
    if (length % PEB_SIZE || length / PEB_SIZE < 3) return "UBI image is not a whole number of PEBs";
    const size_t pebs = length / PEB_SIZE;
    if (pebs > APPFS_PEBS) return "UBI image is larger than the appfs partition";
    uint32_t sequence = 0;
    for (size_t index = 0; index < pebs; index++) {
        const uint8_t *ec = image + index * PEB_SIZE, *vid = ec + VID_HDR_OFFSET;
        if (std::memcmp(ec, "UBI#", 4) || ubi_crc(ec, 60) != be32(ec + 60)) {
            std::snprintf(error, error_size, "PEB %zu: bad EC header", index);
            return error;
        }
        if (be32(ec + 16) != VID_HDR_OFFSET || be32(ec + 20) != DATA_OFFSET) {
            std::snprintf(error, error_size, "PEB %zu: wrong header offsets", index);
            return error;
        }
        if (index == 0) sequence = be32(ec + 24);
        else if (be32(ec + 24) != sequence) return "Mixed UBI image sequence numbers";
        if (std::memcmp(vid, "UBI!", 4) || ubi_crc(vid, 60) != be32(vid + 60)) {
            std::snprintf(error, error_size, "PEB %zu: bad VID header", index);
            return error;
        }
    }
    const uint8_t *vtbl = image + DATA_OFFSET;
    for (size_t slot = 0; slot < VTBL_SLOTS; slot++) {
        const uint8_t *record = vtbl + slot * VTBL_RECORD_SIZE;
        if (ubi_crc(record, 168) != be32(record + 168)) {
            std::snprintf(error, error_size, "Volume table record %zu: bad CRC", slot);
            return error;
        }
    }
    const uint32_t reserved = be32(vtbl);
    if (reserved < pebs - 2 || reserved > APPFS_VOLUME_MAX_PEBS)
        return "Volume 0 size does not fit the image and the appfs partition";
    if (std::memcmp(image + 2 * PEB_SIZE + DATA_OFFSET, "hsqs", 4))
        return "appfs volume is not a little-endian squashfs";
    return nullptr;
}

// A squashfs written straight to the partition, padded to whole erase blocks.
inline const char *check_raw(const uint8_t *image, size_t length, size_t pebs)
{
    constexpr size_t SUPERBLOCK_SIZE = 96;
    if (length == 0 || length % PEB_SIZE) return "appfs image is not a whole number of erase blocks";
    if (length / PEB_SIZE > pebs) return "appfs image is larger than the appfs partition";
    if (std::memcmp(image, "hsqs", 4)) return "appfs image is not a little-endian squashfs";
    const uint64_t used = uint64_t(le32(image + 40)) | uint64_t(le32(image + 44)) << 32;
    if (used < SUPERBLOCK_SIZE || used > length) return "squashfs size does not fit the appfs image";
    return nullptr;
}

// nullptr when the camera's updater would accept the image and the web UI may
// forward it; otherwise a reason (possibly formatted into error).
inline const char *check_appfs_image(const uint8_t *image, size_t length, device_code expected,
                                     char *error, size_t error_size)
{
    if (length < HEADER_SIZE + ENTRY_SIZE + TRAILER_SIZE) return "Image is too short";
    if (length > MAX_IMAGE_SIZE) return "Image is larger than the appfs partition";
    const uint8_t *trailer = image + length - TRAILER_SIZE;
    if (le32(image) != MAGIC || le32(image + 0x30) != MAGIC_END ||
        le32(trailer) != MAGIC || le32(trailer + 24) != MAGIC_END)
        return "Bad header or trailer magic";
    if (le32(image + 0x18) != length) return "File length does not match the header";
    if (le32(image + 4) != TYPE_MTD) return "Unsupported update type";
    if (le32(image + 8) != expected.dev || le32(image + 12) != expected.sdk ||
        le32(image + 16) != expected.platform) {
        std::snprintf(error, error_size, "Device code %u/%u/%u does not match this camera (%u/%u/%u)",
                      unsigned(le32(image + 8)), unsigned(le32(image + 12)), unsigned(le32(image + 16)),
                      unsigned(expected.dev), unsigned(expected.sdk), unsigned(expected.platform));
        return error;
    }
    const appfs_layout *layout = find_layout(expected);
    if (layout == nullptr) {
        std::snprintf(error, error_size, "No known appfs layout for device code %u/%u/%u",
                      unsigned(expected.dev), unsigned(expected.sdk), unsigned(expected.platform));
        return error;
    }
    if (le32(image + 0x14) != 1U) return "Image must contain exactly one partition (appfs)";
    if (image[0x1c] != 1U) return "Image does not reboot after writing";
    // the vendor web uploader requires it, and no known image sets it
    if (image[0x1e] != 0U) return "Unsupported header flags";
    const uint8_t *entry = image + HEADER_SIZE;
    if (!std::memchr(entry, 0, ENTRY_PATH_SIZE) ||
        std::strcmp(reinterpret_cast<const char *>(entry), layout->mtd) != 0) {
        std::snprintf(error, error_size, "Image does not target the appfs partition (%s)", layout->mtd);
        return error;
    }
    const uint32_t data_length = le32(entry + 0x80), offset = le32(entry + 0x84);
    if (offset != HEADER_SIZE + ENTRY_SIZE || size_t(offset) + data_length != length - TRAILER_SIZE)
        return "Partition data does not fill the image";
    if (vendor_crc(image + offset, data_length) != le32(entry + 0x90)) {
        std::snprintf(error, error_size, "%s CRC mismatch", layout->mtd);
        return error;
    }
    if (!layout->ubi) return check_raw(image + offset, data_length, layout->pebs);
    return check_ubi(image + offset, data_length, error, error_size);
}

} // namespace apcam_hisi_firmware
