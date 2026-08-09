#include <gtest/gtest.h>

#include "scanner/scan_types.h"

TEST(ValueParserTest, ParsesInt32) {
    killcore::ScanValue value;
    EXPECT_TRUE(killcore::parseScanValue("41250", killcore::ValueType::Int32, &value));
    EXPECT_EQ(value.value.toInt(), 41250);
}

TEST(ValueParserTest, RejectsInt32Overflow) {
    killcore::ScanValue value;
    EXPECT_FALSE(killcore::parseScanValue("999999999999", killcore::ValueType::Int32, &value));
}

TEST(ValueParserTest, ParsesFloat32) {
    killcore::ScanValue value;
    EXPECT_TRUE(killcore::parseScanValue("12.5", killcore::ValueType::Float32, &value));
    EXPECT_FLOAT_EQ(value.value.toFloat(), 12.5f);
}

TEST(ValueParserTest, ParsesValueTypeAliases) {
    killcore::ValueType type;
    EXPECT_TRUE(killcore::parseValueType("double", &type));
    EXPECT_EQ(type, killcore::ValueType::Float64);
}
