#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmgt911.h"

static dmgt911_config_t screen(void)
{
    dmgt911_config_t c = { .width = 800, .height = 480, .raw_width = 800, .raw_height = 480, .address = 0x5D,
                           .max_contacts = 5, .poll_ms = 10, .release_ms = 300 };
    return c;
}

DMOD_TEST_STEP(validate_accepts_the_board)
{
    dmgt911_config_t c = screen();
    DMOD_TEST_EXPECT_TRUE(dmgt911_validate_config(&c));
    c.raw_width = 0;                        /* the range of the chip */
    c.raw_height = 0;
    DMOD_TEST_EXPECT_TRUE(dmgt911_validate_config(&c));
    c.address = 0x14;
    DMOD_TEST_EXPECT_TRUE(dmgt911_validate_config(&c));
}

DMOD_TEST_STEP(validate_rejects_bad_configurations)
{
    dmgt911_config_t c = screen();
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(NULL));
    c.width = 0;
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(&c));
    c = screen();
    c.address = 0x1A;
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(&c));
    c = screen();
    c.max_contacts = 6;
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(&c));
    c.max_contacts = 0;
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(&c));
    c = screen();
    c.raw_width = 5000;
    DMOD_TEST_EXPECT_FALSE(dmgt911_validate_config(&c));
}

DMOD_TEST_STEP(parse_report_not_ready_says_nothing)
{
    uint8_t report[DMGT911_REPORT_SIZE] = { 0x01, 0x03, 0x20, 0xE0, 0x01 };     /* a contact, but the chip has no new report */
    dmgt911_raw_contact_t c[5];
    bool ready = true;
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 5, &ready), 0);
    DMOD_TEST_EXPECT_FALSE(ready);
}

DMOD_TEST_STEP(parse_report_ready_without_contacts)
{
    uint8_t report[DMGT911_REPORT_SIZE] = { 0x80 };
    dmgt911_raw_contact_t c[5];
    bool ready = false;
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 5, &ready), 0);
    DMOD_TEST_EXPECT_TRUE(ready);                       /* all the fingers are up */
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report) - 1, c, 5, &ready), 0);   /* too short */
    DMOD_TEST_EXPECT_FALSE(ready);
}

DMOD_TEST_STEP(parse_report_one_contact)
{
    /* ready, 1 contact; finger 2 at x = 0x0320 (800), y = 0x01E0 (480), size 0x0030 */
    uint8_t report[DMGT911_REPORT_SIZE] = { 0x81, 0x02, 0x20, 0x03, 0xE0, 0x01, 0x30, 0x00, 0x00 };
    dmgt911_raw_contact_t c[5];
    bool ready = false;
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 5, &ready), 1);
    DMOD_TEST_EXPECT_TRUE(ready);
    DMOD_TEST_EXPECT_EQ(c[0].id, 2);
    DMOD_TEST_EXPECT_EQ(c[0].x, 0x0320);
    DMOD_TEST_EXPECT_EQ(c[0].y, 0x01E0);
    DMOD_TEST_EXPECT_EQ(c[0].pressure, 0x30);
}

DMOD_TEST_STEP(parse_report_two_contacts_and_limits)
{
    uint8_t report[DMGT911_REPORT_SIZE] = { 0x82,
        0x00, 0x0A, 0x00, 0x14, 0x00, 0xFF, 0x01, 0x00,     /* finger 0 at (10, 20), a big contact (0x1FF: cut to 255) */
        0x01, 0x64, 0x00, 0xC8, 0x00, 0x05, 0x00, 0x00 };   /* finger 1 at (100, 200) */
    dmgt911_raw_contact_t c[5];
    bool ready = false;
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 5, &ready), 2);
    DMOD_TEST_EXPECT_EQ(c[0].x, 10);
    DMOD_TEST_EXPECT_EQ(c[0].y, 20);
    DMOD_TEST_EXPECT_EQ(c[0].pressure, 255);
    DMOD_TEST_EXPECT_EQ(c[1].id, 1);
    DMOD_TEST_EXPECT_EQ(c[1].x, 100);
    DMOD_TEST_EXPECT_EQ(c[1].y, 200);
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 1, &ready), 1);     /* the caller takes fewer */
    report[0] = 0x8F;                                                                      /* the chip says more than it has room for */
    DMOD_TEST_EXPECT_EQ(dmgt911_parse_report(report, sizeof(report), c, 5, &ready), 5);
}

DMOD_TEST_STEP(map_point_identity_and_corners)
{
    dmgt911_config_t c = screen();
    uint16_t x, y;
    dmgt911_map_point(&c, 0, 0, &x, &y);
    DMOD_TEST_EXPECT_EQ(x, 0);
    DMOD_TEST_EXPECT_EQ(y, 0);
    dmgt911_map_point(&c, 799, 479, &x, &y);
    DMOD_TEST_EXPECT_EQ(x, 799);
    DMOD_TEST_EXPECT_EQ(y, 479);
    dmgt911_map_point(&c, 5000, 5000, &x, &y);          /* clipped to the screen */
    DMOD_TEST_EXPECT_EQ(x, 799);
    DMOD_TEST_EXPECT_EQ(y, 479);
}

DMOD_TEST_STEP(map_point_mirror_and_swap)
{
    dmgt911_config_t c = screen();
    uint16_t x, y;
    c.mirror_x = true;
    dmgt911_map_point(&c, 0, 0, &x, &y);
    DMOD_TEST_EXPECT_EQ(x, 799);
    DMOD_TEST_EXPECT_EQ(y, 0);
    c = screen();
    c.mirror_y = true;
    dmgt911_map_point(&c, 0, 0, &x, &y);
    DMOD_TEST_EXPECT_EQ(x, 0);
    DMOD_TEST_EXPECT_EQ(y, 479);
    /* a display shown rotated: the screen is 480 x 800, the chip's range 800 x 480 */
    c = screen();
    c.width = 480;
    c.height = 800;
    c.swap_xy = true;
    dmgt911_map_point(&c, 799, 0, &x, &y);             /* the chip's far x is the screen's far y */
    DMOD_TEST_EXPECT_EQ(x, 0);
    DMOD_TEST_EXPECT_EQ(y, 799);
}

DMOD_TEST_STEP(map_point_scales_to_the_screen)
{
    dmgt911_config_t c = screen();
    uint16_t x, y;
    c.width = 400;
    c.height = 240;
    dmgt911_map_point(&c, 799, 479, &x, &y);
    DMOD_TEST_EXPECT_EQ(x, 399);
    DMOD_TEST_EXPECT_EQ(y, 239);
}
