#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <stdexcept>

// Use the real engine type definitions
#include "tjsTypes.h"

// stub for MsgIntf.h — only what CharacterSet.cpp needs
void TVPThrowExceptionMessage(const tjs_char *msg) {
	throw std::runtime_error("TVPThrowExceptionMessage");
}

// pull in the .cpp as a compilation unit
#include "base/CharacterSet.h"
#include "base/CharacterSet.cpp"

TEST(CharacterSet, WideToUtf8_ASCII) {
	tjs_char in[] = TJS_W("hello");
	char out[32] = {};
	tjs_int n = TVPWideCharToUtf8String(in, out);
	EXPECT_EQ(n, 5);
	EXPECT_STREQ(out, "hello");
}

TEST(CharacterSet, WideToUtf8_CJK) {
	tjs_char in[] = TJS_W("\u4e16\u754c"); // 世界
	char out[32] = {};
	tjs_int n = TVPWideCharToUtf8String(in, out);
	EXPECT_EQ(n, 6);
	EXPECT_EQ((unsigned char)out[0], 0xe4);
	EXPECT_EQ((unsigned char)out[1], 0xb8);
	EXPECT_EQ((unsigned char)out[2], 0x96);
	EXPECT_EQ((unsigned char)out[3], 0xe7);
	EXPECT_EQ((unsigned char)out[4], 0x95);
	EXPECT_EQ((unsigned char)out[5], 0x8c);
}

TEST(CharacterSet, WideToUtf8_NullOutput) {
	tjs_char in[] = TJS_W("abc");
	tjs_int n = TVPWideCharToUtf8String(in, nullptr);
	EXPECT_EQ(n, 3);
}

TEST(CharacterSet, WideToUtf8_Empty) {
	tjs_char in[] = TJS_W("");
	char out[32] = {};
	tjs_int n = TVPWideCharToUtf8String(in, out);
	EXPECT_EQ(n, 0);
}

TEST(CharacterSet, Utf8ToWide_ASCII) {
	const char in[] = "hello";
	tjs_char out[32] = {};
	tjs_int n = TVPUtf8ToWideCharString(in, out);
	EXPECT_EQ(n, 5);
	EXPECT_EQ(out[0], TJS_W('h'));
	EXPECT_EQ(out[4], TJS_W('o'));
	EXPECT_EQ(out[5], TJS_W('\0'));
}

TEST(CharacterSet, Utf8ToWide_CJK) {
	const char in[] = "\xe4\xb8\x96\xe7\x95\x8c"; // 世界
	tjs_char out[32] = {};
	tjs_int n = TVPUtf8ToWideCharString(in, out);
	EXPECT_EQ(n, 2);
	EXPECT_EQ(out[0], 0x4e16);
	EXPECT_EQ(out[1], 0x754c);
}

TEST(CharacterSet, Utf8ToWide_NullOutput) {
	const char in[] = "abc";
	tjs_int n = TVPUtf8ToWideCharString(in, nullptr);
	EXPECT_EQ(n, 3);
}

TEST(CharacterSet, Utf8ToWide_Empty) {
	const char in[] = "";
	tjs_char out[32] = {};
	tjs_int n = TVPUtf8ToWideCharString(in, out);
	EXPECT_EQ(n, 0);
}

TEST(CharacterSet, RoundTrip) {
	tjs_char orig[] = TJS_W("Hello \u4e16\u754c! 123");
	char utf8[64] = {};
	tjs_int n = TVPWideCharToUtf8String(orig, utf8);
	ASSERT_GT(n, 0);

	tjs_char decoded[64] = {};
	tjs_int m = TVPUtf8ToWideCharString(utf8, decoded);
	ASSERT_EQ(m, 13); // "Hello 世界! 123" = 13 wide chars
	for (tjs_int i = 0; i < m; ++i)
		EXPECT_EQ(decoded[i], orig[i]);
}

TEST(CharacterSet, Utf8ToWide_ExplicitLength) {
	const char in[] = "abcd";
	tjs_char out[32] = {};
	tjs_int n = TVPUtf8ToWideCharString(in, 2, out);
	EXPECT_EQ(n, 2);
	EXPECT_EQ(out[0], TJS_W('a'));
	EXPECT_EQ(out[1], TJS_W('b'));
	EXPECT_EQ(out[2], TJS_W('\0'));
}

TEST(CharacterSet, Utf8ToWide_InvalidContinuationByte) {
	const char in[] = "\xe4\xb8"; // truncated 3-byte sequence
	tjs_char out[4] = {};
	// invalid bytes are skipped, returns number of valid chars decoded
	tjs_int n = TVPUtf8ToWideCharString(in, out);
	EXPECT_EQ(n, -1); // invalid sequence -> -1
}
