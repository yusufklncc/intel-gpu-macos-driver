// SPDX-License-Identifier: MIT
// probe_css_test: offline check of XeProbe's GuC image header check (xe_probe_css.h), no GPU needed.
//   1. well-formed headers (RSA 2048 as tgl/adlp/dg1, RSA 3072 as dg2/mtl/lnl/ptl/bmg) pass, with the upload size,
//      the RSA size, the version and the private data size Linux xe_uc_fw.c parse_css_header computes
//   2. every malformed header is refused before a size is used: shorter than the header, key sizes that do not add up
//      to the 128-byte header, size_dw under header_size_dw (the old code wrapped around), no ucode, an image shorter
//      than header + ucode + RSA, a private data size past the cap
//   3. with a directory argument: every <stem>_guc_70.bin of firmware/probe-guc.sha256 found there passes, and carries
//      the version that file lists (run after firmware/fetch.sh --probe)
// from kernel/IntelXe: c++ -std=c++17 -Wall -Wextra -I. -o /tmp/probe_css_test tests/probe_css_test.cpp &&
//   /tmp/probe_css_test [FIRMWARE_DIR [PINS (default ../../firmware/probe-guc.sha256)]]
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "xe_probe_css.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// An image as linux-firmware lays it out: the CSS header, ucode, the RSA signature (key_size_dw dwords).
static std::vector<uint8_t> image(uint32_t keyDw, uint32_t ucodeBytes, uint32_t ver, uint32_t priv = 8392704) {
	uc_css_header h = {};
	h.module_type = 0x6;
	h.key_size_dw = keyDw;
	h.modulus_size_dw = keyDw;
	h.exponent_size_dw = 1;
	h.header_size_dw = 32 + 2 * keyDw + 1;
	h.size_dw = h.header_size_dw + ucodeBytes / 4;
	h.sw_version = ver;
	h.private_data_size = priv;
	std::vector<uint8_t> b(sizeof(h) + ucodeBytes + keyDw * 4, 0xa5);
	memcpy(b.data(), &h, sizeof(h));
	return b;
}
static uc_css_header *hdr(std::vector<uint8_t> &b) { return reinterpret_cast<uc_css_header *>(b.data()); }
static bool check(const std::vector<uint8_t> &b, XpCss *c = nullptr) { return xpCssCheck(b.data(), (uint32_t)b.size(), c); }

int main(int argc, char **argv) {
	// 1. well-formed
	for (uint32_t keyDw : {64u, 96u}) {
		auto b = image(keyDw, 0x40000, 70u << 16 | 72u << 8 | 1u);
		XpCss c = {};
		CHECK(check(b, &c), "RSA %u bits refused", keyDw * 32);
		CHECK(c.upload == 128 + 0x40000, "upload %u", c.upload);
		CHECK(c.rsa == keyDw * 4, "rsa %u", c.rsa);
		CHECK(xpVerMajor(c.ver) == 70 && xpVerMinor(c.ver) == 72 && xpVerPatch(c.ver) == 1, "version %08x", c.ver);
		CHECK(c.priv == 8392704, "priv %u", c.priv);
		CHECK(xpCssCheck(b.data(), (uint32_t)b.size(), nullptr), "no out pointer");
		// trailing bytes after the signature are allowed (Linux takes the sizes from the header)
		b.resize(b.size() + 4096, 0);
		CHECK(check(b), "trailing bytes refused");
	}

	// 2. malformed
	{
		auto b = image(96, 0x1000, 0);
		CHECK(!xpCssCheck(b.data(), 127, nullptr), "shorter than the header accepted");
		CHECK(!xpCssCheck(nullptr, 4096, nullptr), "null image accepted");
	}
	{
		auto b = image(96, 0x1000, 0);
		hdr(b)->modulus_size_dw = 95;   // header no longer 128 bytes after the keys
		CHECK(!check(b), "key sizes that do not add up accepted");
	}
	{
		auto b = image(96, 0x1000, 0);
		hdr(b)->key_size_dw = 0xffffffffu;   // keys past header_size_dw: the old subtraction wrapped around
		CHECK(!check(b), "keys larger than the header accepted");
	}
	{
		auto b = image(96, 0x1000, 0);
		hdr(b)->size_dw = hdr(b)->header_size_dw - 1;   // the old (size_dw - header_size_dw) * 4 wrapped to ~16 GB
		CHECK(!check(b), "size_dw under header_size_dw accepted");
	}
	{
		auto b = image(96, 0x1000, 0);
		hdr(b)->size_dw = hdr(b)->header_size_dw;
		CHECK(!check(b), "an image without ucode accepted");
	}
	{
		auto b = image(96, 0x1000, 0);
		b.resize(b.size() - 1);   // the RSA signature one byte short
		CHECK(!check(b), "truncated image accepted");
		b.resize(128 + 0x800);
		CHECK(!check(b), "image cut in the ucode accepted");
	}
	{
		auto b = image(96, 0x1000, 0, kXpCssMaxPriv + 1);
		CHECK(!check(b), "private data size past the cap accepted");
		auto ok = image(96, 0x1000, 0, kXpCssMaxPriv);
		CHECK(check(ok), "private data size at the cap refused");
	}

	// 3. the pinned linux-firmware images, when given
	if (argc > 1) {
		const std::string dir = argv[1], pins = argc > 2 ? argv[2] : "../../firmware/probe-guc.sha256";
		FILE *pf = fopen(pins.c_str(), "r");
		CHECK(pf, "cannot read %s", pins.c_str());
		char line[512];
		int found = 0;
		while (pf && fgets(line, sizeof(line), pf)) {
			char sha[80], path[128], ver[32];
			if (line[0] == '#' || sscanf(line, "%79s %127s %31s", sha, path, ver) != 3) continue;
			const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
			FILE *f = fopen((dir + "/" + name).c_str(), "rb");
			if (!f) continue;
			std::vector<uint8_t> b;
			uint8_t chunk[65536];
			for (size_t k; (k = fread(chunk, 1, sizeof(chunk), f)) > 0;) b.insert(b.end(), chunk, chunk + k);
			fclose(f);
			XpCss c = {};
			CHECK(check(b, &c), "%s refused", name);
			char got[32];
			snprintf(got, sizeof(got), "%u.%u.%u", xpVerMajor(c.ver), xpVerMinor(c.ver), xpVerPatch(c.ver));
			CHECK(!strcmp(got, ver), "%s carries %s, %s lists %s", name, got, pins.c_str(), ver);
			found++;
		}
		if (pf) fclose(pf);
		printf("%d pinned image(s) found in %s\n", found, dir.c_str());
	}

	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
