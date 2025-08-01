/*
 *  Copyright 2024 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "p2p/dtls/dtls_utils.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "api/array_view.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {

TEST(DtlsUtils, GetDtlsHandshakeAcksRejectsTooShort) {
  std::vector<uint8_t> packet = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xde, 0xad  // Length given but bytes not present.
  };
  EXPECT_FALSE(GetDtlsHandshakeAcks(packet));
}

TEST(DtlsUtils, GetDtlsHandshakeAcksRejectsInvalidContent) {
  std::vector<uint8_t> packet = {0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00,
                                 // Correct length given but data is garbage.
                                 0x04, 0xde, 0xad, 0xbe, 0xef};
  EXPECT_FALSE(GetDtlsHandshakeAcks(packet));
}

TEST(DtlsUtils, GetDtlsHandshakeAcksRejectsTrailingData) {
  std::vector<uint8_t> packet = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00,
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Server hello done.
      0xde, 0xad, 0xbe, 0xef                     // Trailing data.
  };
  EXPECT_FALSE(GetDtlsHandshakeAcks(packet));
}

TEST(DtlsUtils, GetDtlsHandshakeAcksBasic) {
  std::vector<uint8_t> packet = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00,
      0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Server hello done.
  };
  std::optional<std::vector<uint16_t>> acks = GetDtlsHandshakeAcks(packet);
  ASSERT_TRUE(acks);
  EXPECT_EQ(acks->size(), 1u);
  EXPECT_THAT(*acks, ::testing::ElementsAreArray({0xac}));
}

TEST(DtlsUtils, GetDtlsHandshakeAcksPackedRecords) {
  // Flight two from server to client but with fragment packing per
  // https://boringssl.googlesource.com/boringssl/+/5245371a08528f7fb21ab20bd7a479d8e395b61c
  std::vector<uint8_t> packet = {
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
      0x43, 0x02, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x60, 0xfe, 0xfd, 0x67, 0x2a, 0x87, 0x84, 0xf8, 0xe5, 0xbc, 0xe5, 0xd1,
      0x2b, 0xfe, 0x53, 0x20, 0xd2, 0xd4, 0x53, 0xa5, 0xbe, 0xd8, 0x38, 0x58,
      0x91, 0xdf, 0x76, 0x21, 0x81, 0x60, 0x7c, 0x6d, 0x8c, 0xdb, 0x93, 0x20,
      0x91, 0xc8, 0xf6, 0x9c, 0xaa, 0xbe, 0x79, 0xa3, 0x28, 0xa6, 0x84, 0xc9,
      0xfa, 0xee, 0x59, 0x22, 0x5d, 0xe2, 0x11, 0x28, 0xf4, 0x80, 0xd6, 0x1a,
      0x3a, 0xb5, 0x3d, 0xb6, 0x61, 0x74, 0xb6, 0x1d, 0xc0, 0x2b, 0x00, 0x00,
      0x18, 0x00, 0x17, 0x00, 0x00, 0xff, 0x01, 0x00, 0x01, 0x00, 0x00, 0x0b,
      0x00, 0x02, 0x01, 0x00, 0x00, 0x0e, 0x00, 0x05, 0x00, 0x02, 0x00, 0x01,
      0x00, 0x0b, 0x00, 0x01, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,
      0x1f, 0x00, 0x01, 0x1c, 0x00, 0x01, 0x19, 0x30, 0x82, 0x01, 0x15, 0x30,
      0x81, 0xbd, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x09, 0x00, 0x9d, 0x2a,
      0x69, 0x9b, 0x1d, 0x5a, 0x38, 0xe5, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86,
      0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x11, 0x31, 0x0f, 0x30, 0x0d,
      0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x06, 0x57, 0x65, 0x62, 0x52, 0x54,
      0x43, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x34, 0x31, 0x31, 0x30, 0x34, 0x32,
      0x31, 0x30, 0x30, 0x35, 0x31, 0x5a, 0x17, 0x0d, 0x32, 0x34, 0x31, 0x32,
      0x30, 0x35, 0x32, 0x31, 0x30, 0x30, 0x35, 0x31, 0x5a, 0x30, 0x11, 0x31,
      0x0f, 0x30, 0x0d, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x06, 0x57, 0x65,
      0x62, 0x52, 0x54, 0x43, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86,
      0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
      0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x1e, 0xd8, 0xad, 0x96, 0x82,
      0xd0, 0xfb, 0xc8, 0xaa, 0xff, 0x84, 0x40, 0x84, 0xfc, 0x1e, 0x4a, 0xfd,
      0x8b, 0xfc, 0x13, 0xbb, 0xee, 0x93, 0xea, 0x91, 0x55, 0x61, 0x7d, 0xc3,
      0x96, 0x66, 0x38, 0x6d, 0x51, 0x59, 0x57, 0xbd, 0xc3, 0xd2, 0x03, 0xf4,
      0xde, 0x48, 0x3f, 0x61, 0x5e, 0x59, 0x2b, 0xfa, 0xfe, 0x68, 0xc0, 0x98,
      0xa3, 0x33, 0xe7, 0xd6, 0xb4, 0x0e, 0xbe, 0x56, 0x48, 0x50, 0x4b, 0x30,
      0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03,
      0x47, 0x00, 0x30, 0x44, 0x02, 0x20, 0x4d, 0xff, 0x9f, 0xf3, 0xc4, 0x08,
      0x15, 0xe0, 0xdd, 0x76, 0x64, 0x0d, 0x50, 0x42, 0x30, 0xbb, 0xf7, 0xca,
      0x78, 0xff, 0xe7, 0x86, 0x05, 0x0f, 0x23, 0x6e, 0xd2, 0x69, 0xd3, 0xc5,
      0xbd, 0xaa, 0x02, 0x20, 0x43, 0x71, 0x52, 0x2f, 0x74, 0x25, 0x78, 0xcd,
      0x62, 0x62, 0x62, 0x0b, 0xbf, 0x76, 0x35, 0xe1, 0xfe, 0x8c, 0x03, 0x6b,
      0x56, 0xb8, 0x96, 0x1f, 0xb1, 0x3a, 0x9f, 0xd9, 0x78, 0x05, 0x66, 0xa7,
      0x0c, 0x00, 0x00, 0x6f, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f,
      0x03, 0x00, 0x1d, 0x20, 0xf5, 0x50, 0xad, 0x14, 0x55, 0xd1, 0xbc, 0x82,
      0xa8, 0xb0, 0x2b, 0x81, 0x3d, 0x18, 0xf4, 0xba, 0x11, 0x54, 0xbb, 0x24,
      0x8b, 0x07, 0xa7, 0x17, 0xf1, 0x33, 0xca, 0x45, 0xc0, 0x6a, 0x16, 0x0a,
      0x04, 0x03, 0x00, 0x47, 0x30, 0x45, 0x02, 0x21, 0x00, 0xad, 0xb6, 0x59,
      0x0c, 0xe0, 0x56, 0x42, 0xb8, 0x9f, 0x40, 0x43, 0xd3, 0x7f, 0x9f, 0xa0,
      0x1d, 0xbc, 0x78, 0xf5, 0xc3, 0x38, 0x99, 0x02, 0xde, 0x11, 0x85, 0x0f,
      0x50, 0xd6, 0x5b, 0x82, 0x7c, 0x02, 0x20, 0x57, 0x2e, 0x0a, 0x82, 0xf7,
      0x14, 0xb6, 0xd6, 0xb2, 0x4b, 0xd7, 0x1a, 0xd6, 0x1b, 0xc2, 0xf6, 0xc2,
      0x4f, 0x3f, 0xe2, 0x8a, 0x06, 0x97, 0xf3, 0x84, 0xc8, 0x60, 0xf1, 0xab,
      0x2d, 0x29, 0xaf, 0x0d, 0x00, 0x00, 0x19, 0x00, 0x03, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x19, 0x02, 0x01, 0x40, 0x00, 0x12, 0x04, 0x03, 0x08, 0x04,
      0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x08, 0x06, 0x06, 0x01,
      0x02, 0x01, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00};

  std::optional<std::vector<uint16_t>> acks = GetDtlsHandshakeAcks(packet);
  ASSERT_TRUE(acks);
  EXPECT_EQ(acks->size(), 5u);
  EXPECT_THAT(*acks, ::testing::ElementsAreArray({0, 1, 2, 3, 4}));

  std::vector<uint8_t> packet2 = {
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x6c, 0x02, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x60, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0xb2, 0xeb, 0x05, 0xc9, 0xba,
      0x39, 0xd0, 0xf6, 0x4b, 0xc9, 0x7e, 0xee, 0x57, 0xfc, 0x2b, 0x90, 0x93,
      0x09, 0xfd, 0x05, 0x80, 0xb3, 0xf5, 0xfc, 0x06, 0x68, 0x38, 0xa8, 0x20,
      0x42, 0x1d, 0x78, 0xe1, 0x97, 0x73, 0x55, 0x0a, 0x16, 0x2d, 0xc1, 0x3e,
      0x4f, 0x71, 0x55, 0xb4, 0x9f, 0xf8, 0x61, 0xe1, 0xbd, 0xe3, 0xf2, 0x2e,
      0x40, 0x29, 0x30, 0x58, 0x37, 0x26, 0x0d, 0xe8, 0xc0, 0x2b, 0x00, 0x00,
      0x18, 0x00, 0x17, 0x00, 0x00, 0xff, 0x01, 0x00, 0x01, 0x00, 0x00, 0x0b,
      0x00, 0x02, 0x01, 0x00, 0x00, 0x0e, 0x00, 0x05, 0x00, 0x02, 0x00, 0x01,
      0x00,  //
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
      0x28, 0x0b, 0x00, 0x01, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,
      0x1c, 0x00, 0x01, 0x19, 0x00, 0x01, 0x16, 0x30, 0x82, 0x01, 0x12, 0x30,
      0x81, 0xb8, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x08, 0x15, 0xc9, 0xcc,
      0xd0, 0x55, 0x57, 0xa2, 0x32, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48,
      0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x0f, 0x31, 0x0d, 0x30, 0x0b, 0x06,
      0x03, 0x55, 0x04, 0x03, 0x0c, 0x04, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1e,
      0x17, 0x0d, 0x32, 0x34, 0x31, 0x31, 0x30, 0x35, 0x32, 0x30, 0x35, 0x33,
      0x34, 0x38, 0x5a, 0x17, 0x0d, 0x32, 0x34, 0x31, 0x32, 0x30, 0x36, 0x32,
      0x30, 0x35, 0x33, 0x34, 0x38, 0x5a, 0x30, 0x0f, 0x31, 0x0d, 0x30, 0x0b,
      0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x04, 0x74, 0x65, 0x73, 0x74, 0x30,
      0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
      0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42,
      0x00, 0x04, 0x8f, 0x43, 0xeb, 0x7b, 0x88, 0x73, 0x4f, 0xe7, 0x69, 0x06,
      0x81, 0xb6, 0xb9, 0xf3, 0xca, 0x73, 0x32, 0x69, 0xb2, 0xc5, 0xe6, 0x4e,
      0xf0, 0x8c, 0xf4, 0xdd, 0x4e, 0x5b, 0xea, 0x06, 0x52, 0x94, 0x9a, 0x12,
      0x77, 0x11, 0xde, 0xf9, 0x12, 0x9a, 0xeb, 0x3c, 0x7c, 0xe4, 0xcf, 0x58,
      0x4c, 0x74, 0x44, 0x84, 0x0a, 0x84, 0xeb, 0xe6, 0xa4, 0xd5, 0xd3, 0x06,
      0xca, 0x52, 0x15, 0x7e, 0xeb, 0x19, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86,
      0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x49, 0x00, 0x30, 0x46, 0x02,
      0x21, 0x00, 0xab, 0xc7, 0x06, 0x7e, 0x36, 0x9b, 0xad, 0xe0, 0x26, 0x61,
      0x6b, 0x59, 0xa0, 0x1c, 0x70, 0x0c, 0xa6, 0xd3, 0xff, 0x8a, 0xc7, 0xba,
      0xe4, 0x23, 0x0a, 0x8b, 0x22, 0x82, 0xcd, 0x5a, 0x5c, 0x56, 0x02, 0x21,
      0x00, 0xc7, 0xe8, 0x57, 0x04, 0xb5, 0x44, 0x69, 0x42, 0xa2, 0xa2, 0x1e,
      0xde, 0x7f, 0xc4, 0x44, 0x98, 0xa4, 0x5c, 0x84, 0x41, 0xa1, 0x31, 0x38,
      0x3c, 0xe5, 0x4f, 0xf5, 0xc0, 0xa9, 0xa8, 0xbc, 0x16,  //
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
      0x7a, 0x0c, 0x00, 0x00, 0x6e, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x6e, 0x03, 0x00, 0x1d, 0x20, 0x89, 0xb9, 0xeb, 0x39, 0x29, 0xa0, 0x31,
      0x08, 0x9a, 0xbf, 0xc3, 0xc0, 0x20, 0x60, 0xbb, 0xea, 0x73, 0x19, 0xcf,
      0x63, 0xe4, 0x5a, 0xa8, 0xa9, 0x56, 0x77, 0xe8, 0x81, 0x48, 0xae, 0x9f,
      0x34, 0x04, 0x03, 0x00, 0x46, 0x30, 0x44, 0x02, 0x20, 0x23, 0x34, 0xc6,
      0x39, 0x94, 0x84, 0xcc, 0x67, 0xeb, 0x44, 0xf9, 0xc3, 0x5c, 0x52, 0xb3,
      0x99, 0x52, 0xf7, 0x4f, 0xff, 0x8b, 0xc5, 0xea, 0xb5, 0xd0, 0xf9, 0x36,
      0xb3, 0xe6, 0xfc, 0x37, 0x50, 0x02, 0x20, 0x4c, 0xe2, 0x29, 0xf5, 0x4a,
      0x4c, 0x7a, 0x01, 0x37, 0xce, 0xc1, 0xb0, 0x15, 0x23, 0xfd, 0xa5, 0xd9,
      0xac, 0x75, 0xcb, 0x55, 0x56, 0x99, 0x97, 0xe3, 0x13, 0xbd, 0x5b, 0xcc,
      0x5d, 0x0c,
      0xa8,  //
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
      0x25, 0x0d, 0x00, 0x00, 0x19, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x19, 0x02, 0x01, 0x40, 0x00, 0x12, 0x04, 0x03, 0x08, 0x04, 0x04, 0x01,
      0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x08, 0x06, 0x06, 0x01, 0x02, 0x01,
      0x00, 0x00,  //
      0x16, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00};

  std::optional<std::vector<uint16_t>> acks2 = GetDtlsHandshakeAcks(packet2);
  ASSERT_TRUE(acks2);
  EXPECT_EQ(acks2->size(), 5u);
  EXPECT_THAT(*acks2, ::testing::ElementsAreArray({0, 1, 2, 3, 4}));
}

TEST(DtlsUtils, GetDtls13HandshakeAcks) {
  // DTLS 1.3 encrypted data, captured with Wireshark. This is a single
  // encrypted record which can not be parsed and should be skipped.
  std::vector<uint8_t> packet = {
      0x2f, 0x5b, 0x4c, 0x00, 0x23, 0x47, 0xab, 0xe7, 0x90, 0x96,
      0xc0, 0xac, 0x2f, 0x25, 0x40, 0x35, 0x35, 0xa3, 0x81, 0x50,
      0x0c, 0x38, 0x0a, 0xf6, 0xd4, 0xd5, 0x7d, 0xbe, 0x9a, 0xa3,
      0xcb, 0xcb, 0x67, 0xb0, 0x77, 0x79, 0x8b, 0x48, 0x60, 0xf8,
  };

  std::optional<std::vector<uint16_t>> acks = GetDtlsHandshakeAcks(packet);
  ASSERT_TRUE(acks);
  EXPECT_EQ(acks->size(), 0u);
}

std::vector<uint8_t> ToVector(ArrayView<const uint8_t> array) {
  return std::vector<uint8_t>(array.begin(), array.end());
}

TEST(PacketStash, Add) {
  PacketStash stash;
  std::vector<uint8_t> packet = {
      0x2f, 0x5b, 0x4c, 0x00, 0x23, 0x47, 0xab, 0xe7, 0x90, 0x96,
      0xc0, 0xac, 0x2f, 0x25, 0x40, 0x35, 0x35, 0xa3, 0x81, 0x50,
      0x0c, 0x38, 0x0a, 0xf6, 0xd4, 0xd5, 0x7d, 0xbe, 0x9a, 0xa3,
      0xcb, 0xcb, 0x67, 0xb0, 0x77, 0x79, 0x8b, 0x48, 0x60, 0xf8,
  };

  stash.Add(packet);
  EXPECT_EQ(stash.size(), 1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet);

  stash.Add(packet);
  EXPECT_EQ(stash.size(), 2);
  EXPECT_EQ(ToVector(stash.GetNext()), packet);
  EXPECT_EQ(ToVector(stash.GetNext()), packet);
}

TEST(PacketStash, AddIfUnique) {
  PacketStash stash;
  std::vector<uint8_t> packet1 = {
      0x2f, 0x5b, 0x4c, 0x00, 0x23, 0x47, 0xab, 0xe7, 0x90, 0x96,
      0xc0, 0xac, 0x2f, 0x25, 0x40, 0x35, 0x35, 0xa3, 0x81, 0x50,
      0x0c, 0x38, 0x0a, 0xf6, 0xd4, 0xd5, 0x7d, 0xbe, 0x9a, 0xa3,
      0xcb, 0xcb, 0x67, 0xb0, 0x77, 0x79, 0x8b, 0x48, 0x60, 0xf8,
  };

  std::vector<uint8_t> packet2 = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00,
      0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };

  stash.AddIfUnique(packet1);
  EXPECT_EQ(stash.size(), 1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);

  stash.AddIfUnique(packet1);
  EXPECT_EQ(stash.size(), 1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);

  stash.AddIfUnique(packet2);
  EXPECT_EQ(stash.size(), 2);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet2);

  stash.AddIfUnique(packet2);
  EXPECT_EQ(stash.size(), 2);
}

TEST(PacketStash, Prune) {
  PacketStash stash;
  std::vector<uint8_t> packet1 = {
      0x2f, 0x5b, 0x4c, 0x00, 0x23, 0x47, 0xab, 0xe7, 0x90, 0x96,
      0xc0, 0xac, 0x2f, 0x25, 0x40, 0x35, 0x35, 0xa3, 0x81, 0x50,
      0x0c, 0x38, 0x0a, 0xf6, 0xd4, 0xd5, 0x7d, 0xbe, 0x9a, 0xa3,
      0xcb, 0xcb, 0x67, 0xb0, 0x77, 0x79, 0x8b, 0x48, 0x60, 0xf8,
  };

  std::vector<uint8_t> packet2 = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00,
      0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };

  stash.AddIfUnique(packet1);
  stash.AddIfUnique(packet2);
  EXPECT_EQ(stash.size(), 2);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet2);

  absl::flat_hash_set<uint32_t> remove;
  remove.insert(PacketStash::Hash(packet1));
  stash.Prune(remove);

  EXPECT_EQ(stash.size(), 1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet2);
}

TEST(PacketStash, PruneSize) {
  PacketStash stash;
  std::vector<uint8_t> packet1 = {
      0x2f, 0x5b, 0x4c, 0x00, 0x23, 0x47, 0xab, 0xe7, 0x90, 0x96,
      0xc0, 0xac, 0x2f, 0x25, 0x40, 0x35, 0x35, 0xa3, 0x81, 0x50,
      0x0c, 0x38, 0x0a, 0xf6, 0xd4, 0xd5, 0x7d, 0xbe, 0x9a, 0xa3,
      0xcb, 0xcb, 0x67, 0xb0, 0x77, 0x79, 0x8b, 0x48, 0x60, 0xf8,
  };

  std::vector<uint8_t> packet2 = {
      0x16, 0xfe, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00,
      0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };

  std::vector<uint8_t> packet3 = {0x3};
  std::vector<uint8_t> packet4 = {0x4};
  std::vector<uint8_t> packet5 = {0x5};
  std::vector<uint8_t> packet6 = {0x6};

  stash.AddIfUnique(packet1);
  stash.AddIfUnique(packet2);
  stash.AddIfUnique(packet3);
  stash.AddIfUnique(packet4);
  stash.AddIfUnique(packet5);
  stash.AddIfUnique(packet6);
  EXPECT_EQ(stash.size(), 6);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet2);
  EXPECT_EQ(ToVector(stash.GetNext()), packet3);
  EXPECT_EQ(ToVector(stash.GetNext()), packet4);
  EXPECT_EQ(ToVector(stash.GetNext()), packet5);
  EXPECT_EQ(ToVector(stash.GetNext()), packet6);

  // Should be NOP.
  stash.Prune(/* max_size= */ 6);
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);
  EXPECT_EQ(ToVector(stash.GetNext()), packet2);
  EXPECT_EQ(ToVector(stash.GetNext()), packet3);
  EXPECT_EQ(ToVector(stash.GetNext()), packet4);
  EXPECT_EQ(ToVector(stash.GetNext()), packet5);
  EXPECT_EQ(ToVector(stash.GetNext()), packet6);

  // Move "cursor" forward.
  EXPECT_EQ(ToVector(stash.GetNext()), packet1);
  stash.Prune(/* max_size= */ 4);
  EXPECT_EQ(stash.size(), 4);
  EXPECT_EQ(ToVector(stash.GetNext()), packet3);
  EXPECT_EQ(ToVector(stash.GetNext()), packet4);
  EXPECT_EQ(ToVector(stash.GetNext()), packet5);
  EXPECT_EQ(ToVector(stash.GetNext()), packet6);
}

}  // namespace webrtc
