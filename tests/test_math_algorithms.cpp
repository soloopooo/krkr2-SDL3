#include <gtest/gtest.h>

extern void DeinterleaveApplyingWindow(float *dest[], const float *src,
                                       float *win, int numch,
                                       size_t destofs, size_t len);

TEST(Deinterleave, Mono) {
	float src[] = {1.0f, 2.0f, 3.0f};
	float win[] = {1.0f, 1.0f, 1.0f};
	float dest_buf[3] = {};
	float *dest[] = {dest_buf};
	DeinterleaveApplyingWindow(dest, src, win, 1, 0, 3);
	EXPECT_FLOAT_EQ(dest_buf[0], 1.0f);
	EXPECT_FLOAT_EQ(dest_buf[1], 2.0f);
	EXPECT_FLOAT_EQ(dest_buf[2], 3.0f);
}

TEST(Deinterleave, Stereo) {
	float src[] = {1.0f, 10.0f, 2.0f, 20.0f, 3.0f, 30.0f};
	float win[] = {0.5f, 0.5f, 0.5f};
	float dest0[3] = {}, dest1[3] = {};
	float *dest[] = {dest0, dest1};
	DeinterleaveApplyingWindow(dest, src, win, 2, 0, 3);
	EXPECT_FLOAT_EQ(dest0[0], 0.5f);
	EXPECT_FLOAT_EQ(dest1[0], 5.0f);
	EXPECT_FLOAT_EQ(dest0[2], 1.5f);
	EXPECT_FLOAT_EQ(dest1[2], 15.0f);
}

TEST(Deinterleave, WindowApplied) {
	float src[] = {2.0f, 4.0f, 6.0f};
	float win[] = {0.5f, 1.0f, 0.25f};
	float dest_buf[3] = {};
	float *dest[] = {dest_buf};
	DeinterleaveApplyingWindow(dest, src, win, 1, 0, 3);
	EXPECT_FLOAT_EQ(dest_buf[0], 1.0f);
	EXPECT_FLOAT_EQ(dest_buf[1], 4.0f);
	EXPECT_FLOAT_EQ(dest_buf[2], 1.5f);
}

TEST(Deinterleave, DestOffset) {
	float src[] = {1.0f, 2.0f, 3.0f};
	float win[] = {1.0f, 1.0f, 1.0f};
	float dest_buf[5] = {-1, -1, -1, -1, -1};
	float *dest[] = {dest_buf};
	DeinterleaveApplyingWindow(dest, src, win, 1, 2, 3);
	EXPECT_FLOAT_EQ(dest_buf[0], -1.0f);
	EXPECT_FLOAT_EQ(dest_buf[1], -1.0f);
	EXPECT_FLOAT_EQ(dest_buf[2], 1.0f);
	EXPECT_FLOAT_EQ(dest_buf[3], 2.0f);
	EXPECT_FLOAT_EQ(dest_buf[4], 3.0f);
}

TEST(Deinterleave, ZeroLength) {
	float src[] = {1.0f};
	float win[] = {1.0f};
	float dest_buf = 999.0f;
	float *dest[] = {&dest_buf};
	DeinterleaveApplyingWindow(dest, src, win, 1, 0, 0);
	EXPECT_FLOAT_EQ(dest_buf, 999.0f);
}
