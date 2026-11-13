// SPDX-License-Identifier: MIT
// xe_probe_css.h: the CSS header check XeProbe makes on a GuC image before stage 3 uploads it, and the version it
// carries (Linux xe_uc_fw.c parse_css_header / uc_fw_parse). Kept apart from xe_probe.cpp so that the host test
// (tests/probe_css_test.cpp) checks it against the linux-firmware images firmware/fetch.sh --probe pins.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "guc_abi.h"

struct XpCss {
	uint32_t upload;   // bytes the DMA copies: the CSS header and the ucode
	uint32_t rsa;      // bytes of the RSA signature after them
	uint32_t ver;      // sw_version: major 23:16, minor 15:8, patch 7:0
	uint32_t priv;     // private_data_size: the GuC's private area after the ADS (8-9 MB in linux-firmware 20261019)
};
static constexpr uint32_t kXpCssMaxPriv = 64u << 20;   // more is not a GuC image the probe should map memory for

// false when the image is too short for its header, the header's sizes do not add up, the image is shorter than
// header + ucode + RSA, or the private data size is past kXpCssMaxPriv. Every size is checked before it is used, so a
// truncated or foreign file cannot make the probe read past the image.
static inline bool xpCssCheck(const uint8_t *image, uint32_t size, XpCss *out) {
	if (!image || size < sizeof(uc_css_header)) return false;
	const uc_css_header *css = reinterpret_cast<const uc_css_header *>(image);
	const uint64_t keys = (uint64_t)css->key_size_dw + css->modulus_size_dw + css->exponent_size_dw;
	if (css->header_size_dw < keys || (css->header_size_dw - keys) * 4 != sizeof(uc_css_header)) return false;
	if (css->size_dw < css->header_size_dw) return false;
	const uint64_t ucode = ((uint64_t)css->size_dw - css->header_size_dw) * 4, rsa = (uint64_t)css->key_size_dw * 4;
	const uint64_t upload = sizeof(uc_css_header) + ucode;
	if (ucode == 0 || upload + rsa > size || css->private_data_size > kXpCssMaxPriv) return false;
	if (out) {
		out->upload = (uint32_t)upload;
		out->rsa = (uint32_t)rsa;
		out->ver = css->sw_version;
		out->priv = css->private_data_size;
	}
	return true;
}

static inline uint32_t xpVerMajor(uint32_t v) { return (v >> 16) & 0xff; }
static inline uint32_t xpVerMinor(uint32_t v) { return (v >> 8) & 0xff; }
static inline uint32_t xpVerPatch(uint32_t v) { return v & 0xff; }
